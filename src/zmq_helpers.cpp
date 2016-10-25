#include "zmq_helpers.hpp"
#include <string>

namespace zmq {

    context_t::context_t(/*TODO: add options*/) {
      //make the c context
      auto* context = zmq_ctx_new();
      if(!context)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(context,
        [](void* context) {
          assert(zmq_ctx_term(context) == 0);
        });
    }
    context_t::operator void*() {
      return ptr.get();
    }

    message_t::message_t(void* data, size_t size, void (*free_function)(void*,void*)) {
      //make the c message
      zmq_msg_t* message = new zmq_msg_t();
      if(zmq_msg_init_data(message, data, size, free_function, nullptr) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(message,
        [](zmq_msg_t* message) {
          assert(zmq_msg_close(message) == 0);
          delete message;
        });
    }
    message_t::message_t(size_t size) {
      //make the c message
      zmq_msg_t* message = new zmq_msg_t();
      if(zmq_msg_init_size(message, size) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(message,
        [](zmq_msg_t* message) {
          assert(zmq_msg_close(message) == 0);
          delete message;
        });
    }
    void message_t::reset(size_t size) {
      assert(zmq_msg_close(ptr.get()) == 0);
      if(zmq_msg_init_size(ptr.get(), size) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    message_t::operator zmq_msg_t*() {
      return ptr.get();
    }
    void* message_t::data() {
      return zmq_msg_data(ptr.get());
    }
    const void* message_t::data() const {
      return zmq_msg_data(const_cast<zmq_msg_t*>(ptr.get()));
    }
    size_t message_t::size() const {
      return zmq_msg_size(const_cast<zmq_msg_t*>(ptr.get()));
    }

    bool message_t::operator==(const message_t& other) const {
      return size() == other.size() && std::memcmp(data(), other.data(), size()) == 0;
    }

    bool message_t::operator!=(const message_t& other) const {
      return size() != other.size() || std::memcmp(data(), other.data(), size()) != 0;
    }

    socket_t::socket_t(const context_t& context, int socket_type):context(context) {
      //make the c socket
      auto* socket = zmq_socket(this->context, socket_type);
      if(!socket)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(socket,
        [](void* socket){
          assert(zmq_close(socket) == 0);
        });
    }
    //set an option on this socket
    void socket_t::setsockopt(int option, const void* value, size_t value_length) {
      if(zmq_setsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //get an option from this socket
    void socket_t::getsockopt(int option, void* value, size_t* value_length) {
      if(zmq_getsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //connect the socket
    void socket_t::connect(const char* address) {
      if(zmq_connect(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //bind the socket
    void socket_t::bind(const char* address) {
      if(zmq_bind(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //read a single message from this socket
    bool socket_t::recv(message_t& message, int flags) {
      auto byte_count = zmq_msg_recv(message, ptr.get(), flags);
      //ignore EAGAIN it just means you asked for non-blocking and there wasn't anything
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //read all of the messages on this socket
    std::list<message_t> socket_t::recv_all(int flags) {
      //grab all message parts
      std::list<message_t> messages;
      int more;
      size_t more_size = sizeof(more);
      do {
        messages.emplace_back();
        if(!recv(messages.back(), flags))
          messages.pop_back();
        getsockopt(ZMQ_RCVMORE, &more, &more_size);
      } while(more);
      return messages;
    }
    //send some bytes
    bool socket_t::send(const void* bytes, size_t count, int flags) {
      auto byte_count = zmq_send(ptr.get(), bytes, count, flags);
      //ignore EAGAIN it just means you asked for non-blocking and we couldnt send the message
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //send a single message
    template <class container_t>
    bool socket_t::send(const container_t& message, int flags) {
      return send(static_cast<const void*>(message.data()), message.size(), flags);
    }
    //send all the messages over this socket
    template <class container_t>
    size_t socket_t::send_all(const std::list<container_t>& messages, int flags) {
      const auto* last_message = &messages.back();
      size_t total = 0;
      for(const auto& message : messages)
        total += static_cast<size_t>(send<container_t>(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | flags));
      return total;
    }
    socket_t::operator void*() {
      return ptr.get();
    }

  //check for events on a bunch of sockets, multiplexing ftw
  int poll(pollitem_t* items, int count, long timeout) {
    int signaled_events;
    if((signaled_events = zmq_poll(items, count, timeout)) < 0)
      throw std::runtime_error(zmq_strerror(zmq_errno()));
    return signaled_events;
  }

  //explicit instantiations for templated sending of data
  template bool socket_t::send<std::string>(const std::string&,int);
  template bool socket_t::send<zmq::message_t>(const zmq::message_t&,int);
  template size_t socket_t::send_all<std::string>(const std::list<std::string>&,int);
  template size_t socket_t::send_all<zmq::message_t>(const std::list<zmq::message_t>&,int);

}

//original at https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
namespace murmur_hash3 {

  //left rotation without carry
  inline uint32_t rotl32 (uint32_t x, int8_t r) { return (x << r) | (x >> (32 - r)); }
  inline uint64_t rotl64 (uint64_t x, int8_t r) { return (x << r) | (x >> (64 - r)); }

  //if your platform needs to do endian-swapping or can only handle aligned reads, do the conversion here
  inline uint32_t getblock32 (const uint32_t * p, int i) { return p[i]; }
  inline uint64_t getblock64 (const uint64_t * p, int i) { return p[i]; }

  //finalization mix to force all bits of a hash block to avalanche
  inline uint32_t fmix32 (uint32_t h) {
    h ^= h >> 16; h *= 0x85ebca6b;
    h ^= h >> 13; h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
  }
  inline uint64_t fmix64 ( uint64_t k ) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdLLU;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53LLU;
    k ^= k >> 33;
    return k;
  }

  //32bit platform with 32bit hash
  uint32_t murmur_hash3_x86_32(const void* key, int len, uint32_t seed) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    //body
    const uint32_t* blocks = static_cast<const uint32_t *>(static_cast<const void*>(data + nblocks * 4));
    for (int i = -nblocks; i; i++) {
      uint32_t k1 = getblock32(blocks, i);
      k1 *= c1;
      k1 = rotl32(k1, 15);
      k1 *= c2;

      h1 ^= k1;
      h1 = rotl32(h1, 13);
      h1 = h1 * 5 + 0xe6546b64;
    }

    //tail
    const uint8_t* tail = static_cast<const uint8_t*>(data + nblocks * 4);
    uint32_t k1 = 0;
    switch (len & 3) {
      case 3:
        k1 ^= tail[2] << 16;
      case 2:
        k1 ^= tail[1] << 8;
      case 1:
        k1 ^= tail[0];
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
    };

    //finalization
    h1 ^= len;
    return fmix32(h1);
  }

  //64bit platform 128bit hash truncated to 64bits
  uint64_t murmur_hash3_x64_128(const void * key, const int len, const uint32_t seed) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    const int nblocks = len / 16;
    uint64_t h1 = seed;
    uint64_t h2 = seed;
    const uint64_t c1 = 0x87c37b91114253d5LLU;
    const uint64_t c2 = 0x4cf5ad432745937fLLU;

    //body
    const uint64_t* blocks = static_cast<const uint64_t*>(static_cast<const void*>(data));
    for (int i = 0; i < nblocks; i++) {
      uint64_t k1 = getblock64(blocks, i * 2 + 0);
      uint64_t k2 = getblock64(blocks, i * 2 + 1);
      k1 *= c1;
      k1 = rotl64(k1, 31);
      k1 *= c2;
      h1 ^= k1;
      h1 = rotl64(h1, 27);
      h1 += h2;
      h1 = h1 * 5 + 0x52dce729;
      k2 *= c2;
      k2 = rotl64(k2, 33);
      k2 *= c1;
      h2 ^= k2;
      h2 = rotl64(h2, 31);
      h2 += h1;
      h2 = h2 * 5 + 0x38495ab5;
    }

    //tail
    const uint8_t * tail = (const uint8_t*) (data + nblocks * 16);
    uint64_t k1 = 0;
    uint64_t k2 = 0;
    switch (len & 15) {
      case 15:
        k2 ^= static_cast<uint64_t>(tail[14]) << 48;
      case 14:
        k2 ^= static_cast<uint64_t>(tail[13]) << 40;
      case 13:
        k2 ^= static_cast<uint64_t>(tail[12]) << 32;
      case 12:
        k2 ^= static_cast<uint64_t>(tail[11]) << 24;
      case 11:
        k2 ^= static_cast<uint64_t>(tail[10]) << 16;
      case 10:
        k2 ^= static_cast<uint64_t>(tail[9]) << 8;
      case 9:
        k2 ^= static_cast<uint64_t>(tail[8]) << 0;
        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;
      case 8:
        k1 ^= static_cast<uint64_t>(tail[7]) << 56;
      case 7:
        k1 ^= static_cast<uint64_t>(tail[6]) << 48;
      case 6:
        k1 ^= static_cast<uint64_t>(tail[5]) << 40;
      case 5:
        k1 ^= static_cast<uint64_t>(tail[4]) << 32;
      case 4:
        k1 ^= static_cast<uint64_t>(tail[3]) << 24;
      case 3:
        k1 ^= static_cast<uint64_t>(tail[2]) << 16;
      case 2:
        k1 ^= static_cast<uint64_t>(tail[1]) << 8;
      case 1:
        k1 ^= static_cast<uint64_t>(tail[0]) << 0;
        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;
    };

    //finalization
    h1 ^= len;
    h2 ^= len;
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;
    return h1; //we only use the first 64 bits of this 128bit hash
  }

  //let the compiler pick which hash makes sense for your architecture
  template<int> size_t specialized_hash_bytes(const void*, size_t);
  template<> size_t specialized_hash_bytes<4>(const void* bytes, size_t length) { return murmur_hash3_x86_32(bytes, length, 1); }
  template<> size_t specialized_hash_bytes<8>(const void* bytes, size_t length) { return murmur_hash3_x64_128(bytes, length, 1); }
  inline size_t hash_bytes(const void* bytes, size_t length) { return specialized_hash_bytes<sizeof(size_t)>(bytes, length); }
}

namespace std {
  size_t hash<zmq::message_t>::operator()(const zmq::message_t& m) const noexcept { return murmur_hash3::hash_bytes(m.data(), m.size()); }
}
