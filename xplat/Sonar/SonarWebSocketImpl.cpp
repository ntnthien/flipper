/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */

#include "SonarWebSocketImpl.h"
#include "SonarStep.h"
#include "ConnectionContextStore.h"
#include "Log.h"
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/async/SSLContext.h>
#include <folly/json.h>
#include <rsocket/Payload.h>
#include <rsocket/RSocket.h>
#include <rsocket/transports/tcp/TcpConnectionFactory.h>
#include <thread>
#include <folly/io/async/AsyncSocketException.h>
#include <stdexcept>

#define WRONG_THREAD_EXIT_MSG \
  "ERROR: Aborting sonar initialization because it's not running in the sonar thread."

static constexpr int reconnectIntervalSeconds = 2;
static constexpr int connectionKeepaliveSeconds = 10;
static constexpr int securePort = 8088;
static constexpr int insecurePort = 8089;

namespace facebook {
namespace sonar {

class ConnectionEvents : public rsocket::RSocketConnectionEvents {
 private:
  SonarWebSocketImpl* websocket_;

 public:
  ConnectionEvents(SonarWebSocketImpl* websocket) : websocket_(websocket) {}

  void onConnected() {
    websocket_->isOpen_ = true;
    if (websocket_->connectionIsTrusted_) {
      websocket_->callbacks_->onConnected();
    }
  }

  void onDisconnected(const folly::exception_wrapper&) {
    if (!websocket_->isOpen_)
      return;
    websocket_->isOpen_ = false;
    if (websocket_->connectionIsTrusted_) {
      websocket_->connectionIsTrusted_ = false;
      websocket_->callbacks_->onDisconnected();
    }
    websocket_->reconnect();
  }

  void onClosed(const folly::exception_wrapper& e) {
    onDisconnected(e);
  }
};

class Responder : public rsocket::RSocketResponder {
 private:
  SonarWebSocketImpl* websocket_;

 public:
  Responder(SonarWebSocketImpl* websocket) : websocket_(websocket) {}

  void handleFireAndForget(
      rsocket::Payload request,
      rsocket::StreamId streamId) {
    const auto payload = request.moveDataToString();
    websocket_->callbacks_->onMessageReceived(folly::parseJson(payload));
  }
};

SonarWebSocketImpl::SonarWebSocketImpl(SonarInitConfig config, std::shared_ptr<SonarState> state, std::shared_ptr<ConnectionContextStore> contextStore)
    : deviceData_(config.deviceData), sonarState_(state), sonarEventBase_(config.callbackWorker), connectionEventBase_(config.connectionWorker), contextStore_(contextStore) {
      CHECK_THROW(config.callbackWorker, std::invalid_argument);
      CHECK_THROW(config.connectionWorker, std::invalid_argument);
    }

SonarWebSocketImpl::~SonarWebSocketImpl() {
  stop();
}

void SonarWebSocketImpl::start() {
  auto step = sonarState_->start("Start connection thread");
  folly::makeFuture()
      .via(sonarEventBase_->getEventBase())
      .delayed(std::chrono::milliseconds(0))
      .thenValue([this, step](auto&&){ step->complete(); startSync(); });
}

void SonarWebSocketImpl::startSync() {
  if (!isRunningInOwnThread()) {
    log(WRONG_THREAD_EXIT_MSG);
    return;
  }
  if (isOpen()) {
    log("Already connected");
    return;
  }
  auto connect = sonarState_->start("Connect to desktop");
  try {
    if (isCertificateExchangeNeeded()) {
      doCertificateExchange();
      return;
    }

    connectSecurely();
    connect->complete();
  } catch (const folly::AsyncSocketException& e) {
    if (e.getType() == folly::AsyncSocketException::NOT_OPEN) {
      // The expected code path when flipper desktop is not running.
      // Don't count as a failed attempt.
      connect->fail("Port not open");
    } else {
      log(e.what());
      failedConnectionAttempts_++;
      connect->fail(e.what());
    }
    reconnect();
  } catch (const std::exception& e) {
    log(e.what());
    connect->fail(e.what());
    failedConnectionAttempts_++;
    reconnect();
  }
}

void SonarWebSocketImpl::doCertificateExchange() {

  rsocket::SetupParameters parameters;
  folly::SocketAddress address;

  parameters.payload = rsocket::Payload(
      folly::toJson(folly::dynamic::object("os", deviceData_.os)(
          "device", deviceData_.device)("app", deviceData_.app)));
  address.setFromHostPort(deviceData_.host, insecurePort);

  auto connectingInsecurely = sonarState_->start("Connect insecurely");
  connectionIsTrusted_ = false;
  client_ =
      rsocket::RSocket::createConnectedClient(
          std::make_unique<rsocket::TcpConnectionFactory>(
              *connectionEventBase_->getEventBase(), std::move(address)),
          std::move(parameters),
          nullptr,
          std::chrono::seconds(connectionKeepaliveSeconds), // keepaliveInterval
          nullptr, // stats
          std::make_shared<ConnectionEvents>(this))
          .get();
  connectingInsecurely->complete();

  requestSignedCertFromSonar();
}

void SonarWebSocketImpl::connectSecurely() {
  rsocket::SetupParameters parameters;
  folly::SocketAddress address;

  auto loadingDeviceId = sonarState_->start("Load Device Id");
  auto deviceId = contextStore_->getDeviceId();
  if (deviceId.compare("unknown")) {
    loadingDeviceId->complete();
  }
  parameters.payload = rsocket::Payload(folly::toJson(folly::dynamic::object(
      "os", deviceData_.os)("device", deviceData_.device)(
      "device_id", deviceId)("app", deviceData_.app)));
  address.setFromHostPort(deviceData_.host, securePort);

  std::shared_ptr<folly::SSLContext> sslContext = contextStore_->getSSLContext();
  auto connectingSecurely = sonarState_->start("Connect securely");
  connectionIsTrusted_ = true;
  client_ =
      rsocket::RSocket::createConnectedClient(
          std::make_unique<rsocket::TcpConnectionFactory>(
              *connectionEventBase_->getEventBase(),
              std::move(address),
              std::move(sslContext)),
          std::move(parameters),
          std::make_shared<Responder>(this),
          std::chrono::seconds(connectionKeepaliveSeconds), // keepaliveInterval
          nullptr, // stats
          std::make_shared<ConnectionEvents>(this))
          .get();
  connectingSecurely->complete();
  failedConnectionAttempts_ = 0;
}

void SonarWebSocketImpl::reconnect() {
  folly::makeFuture()
      .via(sonarEventBase_->getEventBase())
      .delayed(std::chrono::seconds(reconnectIntervalSeconds))
      .thenValue([this](auto&&){ startSync(); });
}

void SonarWebSocketImpl::stop() {
  if (client_) {
    client_->disconnect();
  }
  client_ = nullptr;
}

bool SonarWebSocketImpl::isOpen() const {
  return isOpen_ && connectionIsTrusted_;
}

void SonarWebSocketImpl::setCallbacks(Callbacks* callbacks) {
  callbacks_ = callbacks;
}

void SonarWebSocketImpl::sendMessage(const folly::dynamic& message) {
  sonarEventBase_->add([this, message]() {
    if (client_) {
      client_->getRequester()
          ->fireAndForget(rsocket::Payload(folly::toJson(message)))
          ->subscribe([]() {});
    }
  });
}

bool SonarWebSocketImpl::isCertificateExchangeNeeded() {

  if (failedConnectionAttempts_ >= 2) {
    return true;
  }

  auto step = sonarState_->start("Check required certificates are present");
  bool hasRequiredFiles = contextStore_->hasRequiredFiles();
  if (hasRequiredFiles) {
    step->complete();
  }
  return !hasRequiredFiles;
}

void SonarWebSocketImpl::requestSignedCertFromSonar() {
  auto generatingCSR = sonarState_->start("Generate CSR");
  std::string csr = contextStore_->createCertificateSigningRequest();
  generatingCSR->complete();

  folly::dynamic message = folly::dynamic::object("method", "signCertificate")(
      "csr", csr.c_str())("destination", contextStore_->getCertificateDirectoryPath().c_str());
  auto gettingCert = sonarState_->start("Getting cert from desktop");

  sonarEventBase_->add([this, message, gettingCert]() {
    client_->getRequester()
        ->requestResponse(rsocket::Payload(folly::toJson(message)))
        ->subscribe([this, gettingCert](rsocket::Payload p) {
          auto response = p.moveDataToString();
          if (!response.empty()) {
            folly::dynamic config = folly::parseJson(response);
            contextStore_->storeConnectionConfig(config);
          }
          gettingCert->complete();
          log("Certificate exchange complete.");
          // Disconnect after message sending is complete.
          // This will trigger a reconnect which should use the secure channel.
          // TODO: Connect immediately, without waiting for reconnect
          client_ = nullptr;
        },
        [this, message](folly::exception_wrapper e) {
          e.handle(
            [&](rsocket::ErrorWithPayload& errorWithPayload) {
              std::string errorMessage = errorWithPayload.payload.moveDataToString();

             if (errorMessage.compare("not implemented")) {
               log("Desktop failed to provide certificates. Error from sonar desktop:\n" + errorMessage);
             } else {
              sendLegacyCertificateRequest(message);
             }
            },
            [e](...) {
             log(("Error during certificate exchange:" + e.what()).c_str());
            }
          );
        });
  });
  failedConnectionAttempts_ = 0;
}

void SonarWebSocketImpl::sendLegacyCertificateRequest(folly::dynamic message) {
  // Desktop is using an old version of Flipper.
  // Fall back to fireAndForget, instead of requestResponse.
  auto sendingRequest = sonarState_->start("Sending fallback certificate request");
  client_->getRequester()
   ->fireAndForget(rsocket::Payload(folly::toJson(message)))
   ->subscribe([this, sendingRequest]() {
     sendingRequest->complete();
     client_ = nullptr;
   });
}

bool SonarWebSocketImpl::isRunningInOwnThread() {
  return sonarEventBase_->isInEventBaseThread();
}

} // namespace sonar
} // namespace facebook
