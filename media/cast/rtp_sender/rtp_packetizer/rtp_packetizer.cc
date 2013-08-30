// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/cast/rtp_sender/rtp_packetizer/rtp_packetizer.h"

#include "base/logging.h"
#include "media/cast/cast_defines.h"
#include "media/cast/pacing/paced_sender.h"
#include "net/base/big_endian.h"

namespace media {
namespace cast {

static const uint16 kCommonRtpHeaderLength = 12;
static const uint16 kCastRtpHeaderLength = 7;
static const uint8 kCastKeyFrameBitMask = 0x80;
static const uint8 kCastReferenceFrameIdBitMask = 0x40;

RtpPacketizer::RtpPacketizer(PacedPacketSender* transport,
                             PacketStorage* packet_storage,
                             RtpPacketizerConfig rtp_packetizer_config)
    : config_(rtp_packetizer_config),
      transport_(transport),
      packet_storage_(packet_storage),
      time_last_sent_rtp_timestamp_(0),
      sequence_number_(config_.sequence_number),
      rtp_timestamp_(config_.rtp_timestamp),
      frame_id_(0),
      packet_id_(0),
      send_packets_count_(0),
      send_octet_count_(0) {
  DCHECK(transport) << "Invalid argument";
}

RtpPacketizer::~RtpPacketizer() {}

void RtpPacketizer::IncomingEncodedVideoFrame(
    const EncodedVideoFrame& video_frame,
    int64 capture_time_ms) {
  DCHECK(!config_.audio) << "Invalid state";
  if (config_.audio) return;

  // Timestamp is in 90 KHz for video.
  rtp_timestamp_ = static_cast<uint32>(capture_time_ms * 90);
  time_last_sent_rtp_timestamp_ = capture_time_ms;

  Cast(video_frame.key_frame,
       video_frame.last_referenced_frame_id,
       rtp_timestamp_,
       video_frame.data);
}

void RtpPacketizer::IncomingEncodedAudioFrame(
    const EncodedAudioFrame& audio_frame,
    int64 recorded_time) {
  DCHECK(config_.audio) << "Invalid state";
  if (!config_.audio) return;

  rtp_timestamp_ += audio_frame.samples;  // Timestamp is in samples for audio.
  time_last_sent_rtp_timestamp_ = recorded_time;
  Cast(true, 0, rtp_timestamp_, audio_frame.data);
}

uint16 RtpPacketizer::NextSequenceNumber() {
  ++sequence_number_;
  return sequence_number_ - 1;
}

bool RtpPacketizer::LastSentTimestamp(int64* time_sent,
                                      uint32* rtp_timestamp) const {
  if (time_last_sent_rtp_timestamp_ == 0) return false;

  *time_sent = time_last_sent_rtp_timestamp_;
  *rtp_timestamp = rtp_timestamp_;
  return true;
}

void RtpPacketizer::Cast(bool is_key,
                         uint8 reference_frame_id,
                         uint32 timestamp,
                         std::vector<uint8> data) {
  uint16 rtp_header_length = kCommonRtpHeaderLength + kCastRtpHeaderLength;
  uint16 max_length = config_.max_payload_length - rtp_header_length - 1;
  // Split the payload evenly (round number up).
  uint32 num_packets = (data.size() + max_length) / max_length;
  uint32 payload_length = (data.size() + num_packets) / num_packets;
  DCHECK_LE(payload_length, max_length) << "Invalid argument";

  std::vector<uint8> packet;
  packet.reserve(kIpPacketSize);
  size_t remaining_size = data.size();
  uint8* data_ptr = data.data();
  while (remaining_size > 0) {
    packet.clear();
    if (remaining_size < payload_length) {
      payload_length = remaining_size;
    }
    remaining_size -= payload_length;
    BuildCommonRTPheader(&packet, remaining_size == 0, timestamp);
    // Build Cast header.
    packet.push_back(
        (is_key ? kCastKeyFrameBitMask : 0) | kCastReferenceFrameIdBitMask);
    packet.push_back(frame_id_);
    int start_size = packet.size();
    packet.resize(start_size + 32);
    net::BigEndianWriter big_endian_writer(&((packet)[start_size]), 32);
    big_endian_writer.WriteU16(packet_id_);
    big_endian_writer.WriteU16(num_packets - 1);
    packet.push_back(reference_frame_id);

    // Copy payload data.
    packet.insert(packet.end(), data_ptr, data_ptr + payload_length);
    // Store packet.
    packet_storage_->StorePacket(frame_id_, packet_id_, packet);
    // Send to network.
    transport_->SendPacket(packet, num_packets);
    ++packet_id_;
    data_ptr += payload_length;
    // Update stats.
    ++send_packets_count_;
    send_octet_count_ += payload_length;
  }
  DCHECK(packet_id_ == num_packets) << "Invalid state";
  // Prepare for next frame.
  packet_id_ = 0;
  frame_id_ = static_cast<uint8>(frame_id_ + 1);
}

void RtpPacketizer::BuildCommonRTPheader(
    std::vector<uint8>* packet, bool marker_bit, uint32 time_stamp) {
  packet->push_back(0x80);
  packet->push_back(static_cast<uint8>(config_.payload_type) |
                    (marker_bit ? kRtpMarkerBitMask : 0));
  int start_size = packet->size();
  packet->resize(start_size + 80);
  net::BigEndianWriter big_endian_writer(&((*packet)[start_size]), 80);
  big_endian_writer.WriteU16(sequence_number_);
  big_endian_writer.WriteU32(time_stamp);
  big_endian_writer.WriteU32(config_.ssrc);
  ++sequence_number_;
}

}  // namespace cast
}  // namespace media