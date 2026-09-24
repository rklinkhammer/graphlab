#include <lab_support/message_observation.hpp>
#include <arpa/inet.h>
#include <graphlab/packet_history.hpp>
namespace graphlab::packets {
namespace {
using Bytes = std::span<const unsigned char>;
void require(bool condition) {
  if (!condition)
    throw runtime::Failure("invalid_pcapng");
}
std::uint32_t le(Bytes b, std::size_t n) {
  require(n + 4 <= b.size());
  return std::uint32_t(b[n]) | std::uint32_t(b[n + 1]) << 8 | std::uint32_t(b[n + 2]) << 16 |
         std::uint32_t(b[n + 3]) << 24;
}
unsigned be(Bytes b, std::size_t n) { return unsigned(b[n]) * 256 + b[n + 1]; }
Json protocol(Bytes b, bool messages) {
  Json p = {{"protocol", "other"}, {"decodeStatus", "complete"}};
  auto short_header = [&]() {
    p["decodeStatus"] = "truncated-header";
    return p;
  };
  if (b.size() < 14)
    return short_header();
  unsigned type = be(b, 12);
  std::size_t pos = 14;
  for (int vlan = 0; vlan < 2 && (type == 0x8100 || type == 0x88a8); ++vlan) {
    if (b.size() < pos + 4)
      return short_header();
    type = be(b, pos + 2);
    pos += 4;
  }
  p["etherType"] = type;
  if (type == 0x0806) {
    p["protocol"] = "arp";
    return p;
  }
  unsigned next = 0;
  bool fragment = false;
  std::size_t end = b.size();
  char address[INET6_ADDRSTRLEN];
  if (type == 0x0800) {
    p["protocol"] = "ipv4";
    if (b.size() < pos + 20)
      return short_header();
    auto ihl = std::size_t(b[pos] & 15) * 4, length = std::size_t(be(b, pos + 2));
    if ((b[pos] >> 4) != 4 || ihl < 20 || length < ihl) {
      p["decodeStatus"] = "malformed-header";
      return p;
    }
    if (b.size() < pos + ihl)
      return short_header();
    inet_ntop(AF_INET, b.data() + pos + 12, address, sizeof(address));
    p["sourceAddress"] = address;
    inet_ntop(AF_INET, b.data() + pos + 16, address, sizeof(address));
    p["destinationAddress"] = address;
    next = b[pos + 9];
    fragment = (be(b, pos + 6) & 0x3fff) != 0;
    end = std::min(end, pos + length);
    pos += ihl;
  } else if (type == 0x86dd) {
    p["protocol"] = "ipv6";
    if (b.size() < pos + 40)
      return short_header();
    if ((b[pos] >> 4) != 6) {
      p["decodeStatus"] = "malformed-header";
      return p;
    }
    inet_ntop(AF_INET6, b.data() + pos + 8, address, sizeof(address));
    p["sourceAddress"] = address;
    inet_ntop(AF_INET6, b.data() + pos + 24, address, sizeof(address));
    p["destinationAddress"] = address;
    next = b[pos + 6];
    end = std::min(end, pos + 40 + be(b, pos + 4));
    pos += 40;
    if (next == 0 || next == 43 || next == 44 || next == 50 || next == 51 || next == 60) {
      p["decodeStatus"] = "unsupported-ipv6-extension";
      p["ipProtocol"] = next;
      return p;
    }
  } else {
    p["decodeStatus"] = "unsupported-ethertype";
    return p;
  }
  p["ipProtocol"] = next;
  p["fragmented"] = fragment;
  if (fragment) {
    p["decodeStatus"] = "fragment-no-reassembly";
    return p;
  }
  if (next == 1 || next == 58) {
    p["protocol"] = next == 1 ? "icmp" : "icmpv6";
    return p;
  }
  if (next != 6 && next != 17)
    return p;
  p["protocol"] = next == 6 ? "tcp" : "udp";
  if (end < pos + (next == 6 ? 20 : 8))
    return short_header();
  if ((next == 6 && (b[pos + 12] >> 4) < 5) || (next == 17 && be(b, pos + 4) < 8)) {
    p["decodeStatus"] = "malformed-header";
    return p;
  }
  if (next == 6 && end < pos + std::size_t(b[pos + 12] >> 4) * 4)
    return short_header();
  p["sourcePort"] = be(b, pos);
  p["destinationPort"] = be(b, pos + 2);
  if(messages && type==0x0800 && next==17 && be(b,pos+4)==61 && end>=pos+61) {
    auto id=lab_support::messages::identifier(std::string_view(reinterpret_cast<const char*>(b.data()+pos+8),53));
    if(!id.is_null())p["fixtureIdentifier"]=id;
  }
  return p;
}
} // namespace
Json decode(std::span<const unsigned char> bytes, const Json &context, std::size_t limit) {
  require(limit <= 2000);
  bool section = false, interface = false;
  std::uint32_t snaplen = 0;
  Json items = Json::array();
  std::uint64_t packets = 0, blocks = 0;
  for (std::size_t offset = 0; offset < bytes.size();) {
    require(++blocks <= 1000000 && bytes.size() - offset >= 12);
    auto type = le(bytes, offset), length = le(bytes, offset + 4);
    require(length >= 12 && length % 4 == 0 && length <= 1024 * 1024 &&
            length <= bytes.size() - offset);
    auto b = bytes.subspan(offset, length);
    require(le(b, length - 4) == length);
    if (!section) {
      require(type == 0x0a0d0d0a && length == 28 && le(b, 8) == 0x1a2b3c4d && le(b, 12) == 1);
      section = true;
    } else if (type == 1) {
      require(!interface && length >= 20 && b[8] == 1 && b[9] == 0);
      interface = true;
      snaplen = le(b, 12);
      require(snaplen > 0 && snaplen <= 65535);
      for (std::size_t i = 16; i + 4 <= length - 4;) {
        unsigned code = unsigned(b[i]) + (unsigned(b[i + 1]) << 8),
                 size = unsigned(b[i + 2]) + (unsigned(b[i + 3]) << 8);
        i += 4;
        require(i + size <= length - 4);
        if (!code) {
          require(size == 0);
          break;
        }
        if (code == 9)
          require(size == 1 && b[i] == 6);
        if (code == 14)
          throw runtime::Failure("unsupported_timestamp_offset");
        i += (size + 3) & ~3u;
      }
    } else if (type == 6) {
      require(interface && length >= 32 && le(b, 8) == 0);
      auto captured = le(b, 20), original = le(b, 24);
      require(captured <= original && captured <= snaplen &&
              32ull + ((captured + 3ull) & ~3ull) == length);
      ++packets;
      if (items.size() < limit) {
        auto row = context;
        row["apiVersion"] = "graphlab.packet-observation/v1";
        row["packetIndex"] = std::to_string(packets - 1);
        row["blockOffset"] = std::to_string(offset);
        row["timestampUnixMicros"] = std::to_string((std::uint64_t(le(b, 12)) << 32) | le(b, 16));
        row["timestampSource"] = "pcapng-epb-host-realtime-microseconds";
        row["direction"] = "unknown";
        row["interfaceId"] = "0";
        row["linkType"] = 1;
        row["capturedLength"] = captured;
        row["originalLength"] = original;
        row["truncated"] = captured < original;
        row["headers"] = protocol(b.subspan(28, captured),context.value("decodeMessageIdentifiers",false));
        items.push_back(std::move(row));
      }
    } else if (type == 5) {
      require(interface && length >= 24 && le(b, 8) == 0);
    } else
      throw runtime::Failure("unsupported_pcapng_block");
    offset += length;
  }
  require(section && interface);
  return {{"items", items},
          {"packetCount", std::to_string(packets)},
          {"omittedRecords", std::to_string(packets - items.size())}};
}
} // namespace graphlab::packets
