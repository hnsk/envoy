#include "common/quic/envoy_quic_client_session.h"

#include "common/quic/envoy_quic_utils.h"

namespace Envoy {
namespace Quic {

EnvoyQuicClientSession::EnvoyQuicClientSession(
    const quic::QuicConfig& config, const quic::ParsedQuicVersionVector& supported_versions,
    std::unique_ptr<EnvoyQuicClientConnection> connection, const quic::QuicServerId& server_id,
    quic::QuicCryptoClientConfig* crypto_config,
    quic::QuicClientPushPromiseIndex* push_promise_index, Event::Dispatcher& dispatcher,
    uint32_t send_buffer_limit)
    : QuicFilterManagerConnectionImpl(*connection, dispatcher, send_buffer_limit),
      quic::QuicSpdyClientSession(config, supported_versions, connection.release(), server_id,
                                  crypto_config, push_promise_index),
      host_name_(server_id.host()) {
  // HTTP/3 header limits should be configurable, but for now hard-code to Envoy defaults.
  set_max_inbound_header_list_size(Http::DEFAULT_MAX_REQUEST_HEADERS_KB * 1000);
}

EnvoyQuicClientSession::~EnvoyQuicClientSession() {
  ASSERT(!connection()->connected());
  quic_connection_ = nullptr;
}

absl::string_view EnvoyQuicClientSession::requestedServerName() const { return host_name_; }

void EnvoyQuicClientSession::connect() {
  dynamic_cast<EnvoyQuicClientConnection*>(quic_connection_)->setUpConnectionSocket();
  // Start version negotiation and crypto handshake during which the connection may fail if server
  // doesn't support the one and only supported version.
  CryptoConnect();
  if (quic::VersionUsesHttp3(transport_version())) {
    SetMaxPushId(0u);
  }
}

void EnvoyQuicClientSession::OnConnectionClosed(const quic::QuicConnectionCloseFrame& frame,
                                                quic::ConnectionCloseSource source) {
  quic::QuicSpdyClientSession::OnConnectionClosed(frame, source);
  onConnectionCloseEvent(frame, source);
}

void EnvoyQuicClientSession::Initialize() {
  quic::QuicSpdyClientSession::Initialize();
  quic_connection_->setEnvoyConnection(*this);
}

void EnvoyQuicClientSession::OnCanWrite() {
  if (quic::VersionUsesHttp3(transport_version())) {
    quic::QuicSpdyClientSession::OnCanWrite();
  } else {
    // This will cause header stream flushing. It is the only place to discount bytes buffered in
    // header stream from connection watermark buffer during writing.
    SendBufferMonitor::ScopedWatermarkBufferUpdater updater(headers_stream(), this);
    quic::QuicSpdyClientSession::OnCanWrite();
  }
  maybeApplyDelayClosePolicy();
}

void EnvoyQuicClientSession::OnGoAway(const quic::QuicGoAwayFrame& frame) {
  ENVOY_CONN_LOG(debug, "GOAWAY received with error {}: {}", *this,
                 quic::QuicErrorCodeToString(frame.error_code), frame.reason_phrase);
  quic::QuicSpdyClientSession::OnGoAway(frame);
  if (http_connection_callbacks_ != nullptr) {
    http_connection_callbacks_->onGoAway(quicErrorCodeToEnvoyErrorCode(frame.error_code));
  }
}

void EnvoyQuicClientSession::OnHttp3GoAway(uint64_t stream_id) {
  ENVOY_CONN_LOG(debug, "HTTP/3 GOAWAY received", *this);
  quic::QuicSpdyClientSession::OnHttp3GoAway(stream_id);
  if (http_connection_callbacks_ != nullptr) {
    // HTTP/3 GOAWAY doesn't have an error code field.
    http_connection_callbacks_->onGoAway(Http::GoAwayErrorCode::NoError);
  }
}

void EnvoyQuicClientSession::SetDefaultEncryptionLevel(quic::EncryptionLevel level) {
  quic::QuicSpdyClientSession::SetDefaultEncryptionLevel(level);
  if (level == quic::ENCRYPTION_FORWARD_SECURE) {
    // This is only reached once, when handshake is done.
    raiseConnectionEvent(Network::ConnectionEvent::Connected);
  }
}

std::unique_ptr<quic::QuicSpdyClientStream> EnvoyQuicClientSession::CreateClientStream() {
  ASSERT(codec_stats_.has_value() && http3_options_.has_value());
  return std::make_unique<EnvoyQuicClientStream>(GetNextOutgoingBidirectionalStreamId(), this,
                                                 quic::BIDIRECTIONAL, codec_stats_.value(),
                                                 http3_options_.value());
}

quic::QuicSpdyStream* EnvoyQuicClientSession::CreateIncomingStream(quic::QuicStreamId /*id*/) {
  // Disallow server initiated stream.
  NOT_REACHED_GCOVR_EXCL_LINE;
}

quic::QuicSpdyStream*
EnvoyQuicClientSession::CreateIncomingStream(quic::PendingStream* /*pending*/) {
  // Disallow server initiated stream.
  NOT_REACHED_GCOVR_EXCL_LINE;
}

bool EnvoyQuicClientSession::hasDataToWrite() { return HasDataToWrite(); }

void EnvoyQuicClientSession::OnTlsHandshakeComplete() {
  raiseConnectionEvent(Network::ConnectionEvent::Connected);
}

size_t EnvoyQuicClientSession::WriteHeadersOnHeadersStream(
    quic::QuicStreamId id, spdy::SpdyHeaderBlock headers, bool fin,
    const spdy::SpdyStreamPrecedence& precedence,
    quic::QuicReferenceCountedPointer<quic::QuicAckListenerInterface> ack_listener) {
  ASSERT(!quic::VersionUsesHttp3(transport_version()));
  // gQUIC headers are sent on a dedicated stream. Only count the bytes sent against
  // connection level watermark buffer. Do not count them into stream level
  // watermark buffer, because it is impossible to identify which byte belongs
  // to which stream when the buffered bytes are drained in headers stream.
  // This updater may be in the scope of another one in OnCanWrite(), in such
  // case, this one doesn't update the watermark.
  SendBufferMonitor::ScopedWatermarkBufferUpdater updater(headers_stream(), this);
  return quic::QuicSpdyClientSession::WriteHeadersOnHeadersStream(id, std::move(headers), fin,
                                                                  precedence, ack_listener);
}

} // namespace Quic
} // namespace Envoy
