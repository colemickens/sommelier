// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hermes/card_qrtr.h"

#include <algorithm>
#include <array>
#include <utility>

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <libqrtr.h>

#include "hermes/apdu.h"
#include "hermes/qmi_uim.h"
#include "hermes/sgp_22.h"
#include "hermes/socket_qrtr.h"

namespace {

// As per QMI UIM spec section 2.2
constexpr uint8_t kQmiUimService = 0xB;
// TODO(jruthe): this is currently the slot on Cheza, but Esim should be able to
// support different slots in the future.
constexpr uint8_t kEsimSlot = 0x01;
constexpr uint8_t kInvalidChannel = -1;

bool DecodeSuccessful(const uim_qmi_result& qmi_result) {
  return (qmi_result.result == 0);
}

}  // namespace

namespace hermes {

struct ApduTxInfo : public CardQrtr::TxInfo {
  explicit ApduTxInfo(CommandApdu apdu, CardQrtr::ResponseCallback cb = nullptr)
      : apdu_(std::move(apdu)), callback_(cb) {}
  CommandApdu apdu_;
  CardQrtr::ResponseCallback callback_;
};

std::unique_ptr<CardQrtr> CardQrtr::Create(
    std::unique_ptr<SocketInterface> socket,
    Logger* logger,
    Executor* executor) {
  // Open the socket prior to passing to CardQrtr, such that it always has a
  // valid socket to write to.
  if (!socket || !socket->Open()) {
    LOG(ERROR) << "Failed to open socket";
    return nullptr;
  }
  return std::unique_ptr<CardQrtr>(
      new CardQrtr(std::move(socket), logger, executor));
}

CardQrtr::CardQrtr(std::unique_ptr<SocketInterface> socket,
                   Logger* logger,
                   Executor* executor)
    : extended_apdu_supported_(false),
      current_transaction_id_(static_cast<uint16_t>(-1)),
      channel_(kInvalidChannel),
      slot_(kEsimSlot),
      socket_(std::move(socket)),
      buffer_(4096),
      logger_(logger),
      executor_(executor) {
  CHECK(socket_);
  CHECK(socket_->IsValid());
  socket_->SetDataAvailableCallback(
      base::Bind(&CardQrtr::OnDataAvailable, base::Unretained(this)));

  // Set SGP.22 specification version supported by this implementation (this is
  // not currently constrained by the eUICC we use).
  spec_version_.set_major(2);
  spec_version_.set_minor(2);
  spec_version_.set_revision(0);
}

CardQrtr::~CardQrtr() {
  Shutdown();
  socket_->Close();
}

void CardQrtr::SendApdus(std::vector<lpa::card::Apdu> apdus,
                         ResponseCallback cb) {
  if (current_state_ == State::kUninitialized) {
    Initialize();
  }
  for (size_t i = 0; i < apdus.size(); ++i) {
    ResponseCallback callback =
        (i == apdus.size() - 1 ? std::move(cb) : nullptr);
    CommandApdu apdu(static_cast<ApduClass>(apdus[i].cla()),
                     static_cast<ApduInstruction>(apdus[i].ins()),
                     extended_apdu_supported_);
    apdu.AddData(apdus[i].data());
    tx_queue_.push_back(
        {std::make_unique<ApduTxInfo>(std::move(apdu), std::move(callback)),
         AllocateId(), QmiUimCommand::kSendApdu});
  }
  // Begin transmitting if we are not already processing a transaction.
  if (current_state_ == State::kReady) {
    TransmitFromQueue();
  }
}

bool CardQrtr::IsSimValidAfterEnable() {
  return true;
}

bool CardQrtr::IsSimValidAfterDisable() {
  return true;
}

void CardQrtr::Initialize() {
  CHECK(socket_->IsValid());

  if (current_state_.IsInitialized()) {
    LOG(WARNING)
        << "Attempt to initialize already-initialized CardQrtr instance";
    return;
  } else if (current_state_ != State::kUninitialized) {
    LOG(WARNING) << "Attempt to initialize a CardQrtr instance that is already "
                 << "initializing";
    return;
  }

  current_state_.Transition(State::kInitializeStarted);
  // StartService should result in a received QRTR_TYPE_NEW_SERVER
  // packet. Don't send other packets until that occurs.
  if (!socket_->StartService(kQmiUimService, 1, 0)) {
    LOG(ERROR) << "Starting uim service failed during CardQrtr initialization";
    return;
  }
  // Place Reset request on the tx queue
  tx_queue_.push_back(
      {std::unique_ptr<TxInfo>(), AllocateId(), QmiUimCommand::kReset});
  // Place OpenLogicalChannel request on the tx queue
  tx_queue_.push_back({std::unique_ptr<TxInfo>(), AllocateId(),
                       QmiUimCommand::kOpenLogicalChannel});
}

void CardQrtr::FinalizeInitialization() {
  if (current_state_ != State::kLogicalChannelOpened) {
    LOG(ERROR) << "CardQrtr initialization unsuccessful";
    // TODO(akhouderchah) Call lpa library and flush kSendApdu requests from the
    // queue.
    Shutdown();
    return;
  }
  LOG(INFO) << "CardQrtr initialization successful";
  current_state_.Transition(State::kReady);
  // TODO(akhouderchah) set this based on whether or not Extended Length APDU is
  // supported.
  extended_apdu_supported_ = false;
}

void CardQrtr::Shutdown() {
  if (current_state_ != State::kUninitialized &&
      current_state_ != State::kInitializeStarted) {
    // TODO(akhouderchah) Implement actual shutdown procedure
    socket_->StopService(kQmiUimService, 1, 0);
  }
  current_state_.Transition(State::kUninitialized);
}

uint16_t CardQrtr::AllocateId() {
  // transaction id cannot be 0, but when incrementing by 1, an overflow will
  // result in this method at some point returning 0. Incrementing by 2 when
  // transaction_id is initialized as an odd number guarantees us that this
  // method will never return 0 without special-casing the overflow.
  current_transaction_id_ += 2;
  return current_transaction_id_;
}

/////////////////////////////////////
// Transmit method implementations //
/////////////////////////////////////

void CardQrtr::TransmitFromQueue() {
  if (tx_queue_.empty()) {
    return;
  }

  bool should_pop = true;
  switch (tx_queue_[0].uim_type_) {
    case QmiUimCommand::kReset:
      TransmitQmiReset(&tx_queue_[0]);
      break;
    case QmiUimCommand::kOpenLogicalChannel:
      TransmitQmiOpenLogicalChannel(&tx_queue_[0]);
      current_state_.Transition(State::kLogicalChannelPending);
      break;
    case QmiUimCommand::kSendApdu:
      // kSendApdu element will be popped off the queue after the response has
      // been entirely received. This occurs within |ReceiveQmiSendApdu|.
      should_pop = false;
      TransmitQmiSendApdu(&tx_queue_[0]);
      break;
    default:
      LOG(ERROR) << "Unexpected QMI UIM type in CardQrtr tx queue";
  }
  if (should_pop) {
    tx_queue_.pop_front();
  }
}

void CardQrtr::TransmitQmiReset(TxElement* tx_element) {
  DCHECK(tx_element && tx_element->uim_type_ == QmiUimCommand::kReset);

  uim_reset_req request;
  SendCommand(QmiUimCommand::kReset, tx_element->id_, &request,
              uim_reset_req_ei);
}

void CardQrtr::TransmitQmiOpenLogicalChannel(TxElement* tx_element) {
  DCHECK(tx_element &&
         tx_element->uim_type_ == QmiUimCommand::kOpenLogicalChannel);

  uim_open_logical_channel_req request;
  request.slot = slot_;
  request.aid_valid = true;
  request.aid_len = kAidIsdr.size();
  std::copy(kAidIsdr.begin(), kAidIsdr.end(), request.aid);

  SendCommand(QmiUimCommand::kOpenLogicalChannel, tx_element->id_, &request,
              uim_open_logical_channel_req_ei);
}

void CardQrtr::TransmitQmiSendApdu(TxElement* tx_element) {
  DCHECK(tx_element && tx_element->uim_type_ == QmiUimCommand::kSendApdu);

  // TODO(akhouderchah) we can't avoid the copy when encoding into QMI format,
  // but there is really no need to have this copy. Fix.
  uim_send_apdu_req request;
  request.slot = slot_;
  request.channel_id_valid = true;
  request.channel_id = channel_;

  uint8_t* fragment;
  ApduTxInfo* apdu = static_cast<ApduTxInfo*>(tx_element->info_.get());
  size_t fragment_size = apdu->apdu_.GetNextFragment(&fragment);
  request.apdu_len = fragment_size;
  std::copy(fragment, fragment + fragment_size, request.apdu);

  SendCommand(QmiUimCommand::kSendApdu, tx_element->id_, &request,
              uim_send_apdu_req_ei);
}

bool CardQrtr::SendCommand(QmiUimCommand type,
                           uint16_t id,
                           void* c_struct,
                           qmi_elem_info* ei) {
  if (current_state_ == State::kWaitingForResponse) {
    LOG(ERROR) << "CardQrtr: attempt to send raw buffer when waiting for a "
               << "response";
    return false;
  } else if (!current_state_.CanSend()) {
    LOG(ERROR) << "QRTR tried to send buffer in a non-sending state: "
               << current_state_;
    return false;
  } else if (!socket_->IsValid()) {
    LOG(ERROR) << "CardQrtr socket is invalid!";
    return false;
  }

  std::vector<uint8_t> encoded_buffer(kBufferDataSize * 2, 0);
  qrtr_packet packet;
  packet.data = encoded_buffer.data();
  packet.data_len = encoded_buffer.size();

  size_t len = qmi_encode_message(
      &packet, QMI_REQUEST, static_cast<uint16_t>(type), id, c_struct, ei);
  if (len < 0) {
    LOG(ERROR) << "Failed to encode QMI UIM request: "
               << static_cast<uint16_t>(type);
    return false;
  }

  VLOG(1) << "CardQrtr sending transaction type " << static_cast<uint16_t>(type)
          << " with data (size : " << packet.data_len
          << ") : " << base::HexEncode(packet.data, packet.data_len);
  int success = socket_->Send(packet.data, packet.data_len,
                              reinterpret_cast<void*>(&metadata_));
  if (success < 0) {
    LOG(ERROR) << "qrtr_sendto failed";
    return false;
  }
  // If we are sending a command as per the initialization sequence
  // (e.g. sending an OPEN_LOGICAL_CHANNEL request), we do not want to jump
  // straight to kWaitingForResponse afterwards.
  if (current_state_ == State::kReady) {
    current_state_.Transition(State::kWaitingForResponse);
  }
  return true;
}

////////////////////////////////////
// Receive method implementations //
////////////////////////////////////

void CardQrtr::ProcessQrtrPacket(uint32_t node, uint32_t port, int size) {
  sockaddr_qrtr qrtr_sock;
  qrtr_sock.sq_family = AF_QIPCRTR;
  qrtr_sock.sq_node = node;
  qrtr_sock.sq_port = port;

  qrtr_packet pkt;
  int ret = qrtr_decode(&pkt, buffer_.data(), size, &qrtr_sock);
  if (ret < 0) {
    LOG(ERROR) << "qrtr_decode failed";
    return;
  }

  switch (pkt.type) {
    case QRTR_TYPE_NEW_SERVER:
      VLOG(1) << "Received NEW_SERVER QRTR packet";
      if (pkt.service == kQmiUimService && channel_ == kInvalidChannel) {
        current_state_.Transition(State::kUimStarted);
        metadata_.node = pkt.node;
        metadata_.port = pkt.port;
      }
      break;
    case QRTR_TYPE_DATA:
      if (current_state_ == State::kWaitingForResponse) {
        current_state_.Transition(State::kReady);
      }
      VLOG(1) << "Received data QRTR packet";
      ProcessQmiPacket(pkt);
      break;
    case QRTR_TYPE_DEL_SERVER:
    case QRTR_TYPE_HELLO:
    case QRTR_TYPE_BYE:
    case QRTR_TYPE_DEL_CLIENT:
    case QRTR_TYPE_RESUME_TX:
    case QRTR_TYPE_EXIT:
    case QRTR_TYPE_PING:
    case QRTR_TYPE_NEW_LOOKUP:
    case QRTR_TYPE_DEL_LOOKUP:
      LOG(INFO) << "Received QRTR packet of type " << pkt.type << ". Ignoring.";
      break;
    default:
      LOG(WARNING) << "Received QRTR packet but did not recognize packet type "
                   << pkt.type << ".";
  }
  // If we cannot yet send another request, it is because we are waiting for a
  // response. After the response is received and processed, the next request
  // will be sent.
  if (current_state_.CanSend()) {
    TransmitFromQueue();
  }
}

void CardQrtr::ProcessQmiPacket(const qrtr_packet& packet) {
  uint32_t qmi_type;
  if (qmi_decode_header(&packet, &qmi_type) < 0) {
    LOG(ERROR) << "QRTR received invalid QMI packet";
    return;
  }

  // TODO(akhouderchah) try to avoid the unnecessary copy from *_resp to vector
  switch (static_cast<QmiUimCommand>(qmi_type)) {
    case QmiUimCommand::kReset:
      // TODO(akhouderchah) implement a service reset
      break;
    case QmiUimCommand::kOpenLogicalChannel:
      ReceiveQmiOpenLogicalChannel(packet);
      if (!current_state_.IsInitialized()) {
        FinalizeInitialization();
      }
      break;
    case QmiUimCommand::kSendApdu:
      ReceiveQmiSendApdu(packet);
      break;
    default:
      LOG(WARNING) << "Received QMI packet of unknown type: " << qmi_type;
  }
}

void CardQrtr::ReceiveQmiOpenLogicalChannel(const qrtr_packet& packet) {
  uim_open_logical_channel_resp resp;
  unsigned int id;
  if (qmi_decode_message(
          &resp, &id, &packet, QMI_RESPONSE,
          static_cast<uint16_t>(QmiUimCommand::kOpenLogicalChannel),
          uim_open_logical_channel_resp_ei) < 0) {
    LOG(ERROR) << "Failed to decode QMI UIM response kOpenLogicalChannel";
    return;
  }
  if (current_state_ != State::kLogicalChannelPending) {
    LOG(ERROR) << "Received unexpected QMI UIM response: "
               << "kOpenLogicalChannel in state " << current_state_;
    return;
  }
  if (!DecodeSuccessful(resp.result)) {
    LOG(ERROR) << "kOpenLogicalChannel response indicating error";
    return;
  }
  if (!resp.channel_id_valid) {
    LOG(ERROR) << "QMI UIM response for kOpenLogicalChannel contained an "
               << "invalid channel id";
    return;
  }

  channel_ = resp.channel_id;
  current_state_.Transition(State::kLogicalChannelOpened);
}

void CardQrtr::ReceiveQmiSendApdu(const qrtr_packet& packet) {
  CHECK(tx_queue_.size());
  // Ensure that the queued element is for a kSendApdu command
  TxInfo* base_info = tx_queue_[0].info_.get();
  CHECK(base_info);
  CHECK(dynamic_cast<ApduTxInfo*>(base_info));

  static ResponseApdu payload;
  uim_send_apdu_resp resp;
  unsigned int id;
  ApduTxInfo* info = static_cast<ApduTxInfo*>(base_info);
  qmi_decode_message(&resp, &id, &packet, QMI_RESPONSE,
                     static_cast<uint16_t>(QmiUimCommand::kSendApdu),
                     uim_send_apdu_resp_ei);
  if (!DecodeSuccessful(resp.result)) {
    LOG(ERROR) << "Failed to decode received QMI UIM response: kSendApdu";
    return;
  } else if (resp.result.error) {
    LOG(ERROR) << "kSendApdu response contained error: " << resp.result.error;
    return;
  }

  VLOG(2) << "Adding to payload from APDU response ("
          << resp.apdu_response_len - 2 << " bytes): "
          << base::HexEncode(resp.apdu_response, resp.apdu_response_len - 2);
  payload.AddData(resp.apdu_response, resp.apdu_response_len);
  if (payload.MorePayloadIncoming()) {
    // Make the next transmit operation be a request for more APDU data
    info->apdu_ = payload.CreateGetMoreCommand(false);
    return;
  } else if (info->apdu_.HasMoreFragments()) {
    // Send next fragment of APDU
    VLOG(1) << "Sending next APDU fragment...";
    TransmitFromQueue();
    return;
  }

  if (tx_queue_.empty() || static_cast<uint16_t>(id) != tx_queue_[0].id_) {
    LOG(ERROR) << "CardQrtr received APDU from modem with unrecognized "
               << "transaction ID";
    return;
  }

  VLOG(1) << "Finished transaction " << tx_queue_[0].id_ / 2
          << " (id: " << tx_queue_[0].id_ << ")";
  responses_.push_back(payload.Release());
  if (info->callback_) {
    info->callback_(responses_, lpa::card::EuiccCard::kNoError);
    // ResponseCallback interface does not indicate a change in ownership of
    // |responses_|, but all callbacks should transfer ownership. Check for
    // sanity.
    CHECK(responses_.empty());
  }
  tx_queue_.pop_front();
}

void CardQrtr::OnDataAvailable(SocketInterface* socket) {
  CHECK(socket == socket_.get());

  void* metadata = nullptr;
  SocketQrtr::PacketMetadata data = {0, 0};
  if (socket->GetType() == SocketInterface::Type::kQrtr) {
    metadata = reinterpret_cast<void*>(&data);
  }

  int bytes_received = socket->Recv(buffer_.data(), buffer_.size(), metadata);
  if (bytes_received < 0) {
    LOG(ERROR) << "Socket recv failed";
    return;
  }
  VLOG(2) << "CardQrtr recevied raw data (" << bytes_received
          << " bytes): " << base::HexEncode(buffer_.data(), bytes_received);
  LOG(INFO) << data.port;
  ProcessQrtrPacket(data.node, data.port, bytes_received);
}

const lpa::proto::EuiccSpecVersion& CardQrtr::GetCardVersion() {
  return spec_version_;
}

bool CardQrtr::State::Transition(CardQrtr::State::Value value) {
  bool valid_transition = false;
  switch (value) {
    case kUninitialized:
      valid_transition = true;
      break;
    case kReady:
      valid_transition =
          (value_ == kLogicalChannelOpened || value_ == kWaitingForResponse);
      break;
    default:
      // Most states can only transition from the previous state.
      valid_transition = (value == value_ + 1);
  }

  if (valid_transition) {
    value_ = value;
  } else {
    LOG(ERROR) << "Cannot transition from state " << *this << " to state "
               << State(value);
  }
  return valid_transition;
}

}  // namespace hermes
