#include "hci/le_address_rotator.h"
#include "os/log.h"
#include "os/rand.h"

namespace bluetooth {
namespace hci {

static constexpr uint8_t BLE_ADDR_MASK = 0xc0;

LeAddressRotator::LeAddressRotator(common::Callback<void(Address address)> set_random_address, os::Handler* handler,
                                   Address public_address) {
  set_random_address_ = set_random_address;
  handler_ = handler;
  public_address_ = AddressWithType(public_address, AddressType::PUBLIC_DEVICE_ADDRESS);
};

LeAddressRotator::~LeAddressRotator() {
  if (address_rotation_alarm_ != nullptr) {
    address_rotation_alarm_->Cancel();
    address_rotation_alarm_.reset();
  }
}

// Aborts if called more than once
void LeAddressRotator::SetPrivacyPolicyForInitiatorAddress(AddressPolicy address_policy, AddressWithType fixed_address,
                                                           crypto_toolbox::Octet16 rotation_irk,
                                                           std::chrono::milliseconds minimum_rotation_time,
                                                           std::chrono::milliseconds maximum_rotation_time) {
  ASSERT(address_policy_ == AddressPolicy::POLICY_NOT_SET);
  ASSERT(address_policy != AddressPolicy::POLICY_NOT_SET);
  address_policy_ = address_policy;

  switch (address_policy_) {
    case AddressPolicy::USE_PUBLIC_ADDRESS:
    case AddressPolicy::USE_STATIC_ADDRESS:
      fixed_address_ = fixed_address;
      resume_registered_clients();
      break;
    case AddressPolicy::USE_NON_RESOLVABLE_ADDRESS:
    case AddressPolicy::USE_RESOLVABLE_ADDRESS:
      rotation_irk_ = rotation_irk;
      minimum_rotation_time_ = minimum_rotation_time;
      maximum_rotation_time_ = maximum_rotation_time;
      address_rotation_alarm_ = std::make_unique<os::Alarm>(handler_);

      if (registered_clients_.empty()) {
        return;
      }

      for (auto client : registered_clients_) {
        if (client.second != ClientState::PAUSED) {
          // make sure all client paused
          return;
        }
      }
      rotate_random_address();
      break;
    default:
      LOG_ALWAYS_FATAL("invalid parameters");
  }
}

void LeAddressRotator::Register(LeAddressRotatorCallback* callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  registered_clients_.insert(std::pair<LeAddressRotatorCallback*, ClientState>(callback, ClientState::RESUMED));
  if (address_policy_ == AddressPolicy::POLICY_NOT_SET || address_policy_ == AddressPolicy::USE_RESOLVABLE_ADDRESS ||
      address_policy_ == AddressPolicy::USE_NON_RESOLVABLE_ADDRESS) {
    pause_registered_clients();
  }
}

void LeAddressRotator::Unregister(LeAddressRotatorCallback* callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  registered_clients_.erase(callback);
  if (registered_clients_.empty() && address_rotation_alarm_ != nullptr) {
    address_rotation_alarm_->Cancel();
    address_rotation_alarm_.reset();
  }
}

void LeAddressRotator::AckPause(LeAddressRotatorCallback* callback) {
  handler_->Post(common::BindOnce(&LeAddressRotator::ack_pause, common::Unretained(this), callback));
}

void LeAddressRotator::AckResume(LeAddressRotatorCallback* callback) {
  handler_->Post(common::BindOnce(&LeAddressRotator::ack_resume, common::Unretained(this), callback));
}

void LeAddressRotator::OnLeSetRandomAddressComplete(bool success) {
  ASSERT(success);
  resume_registered_clients();
}

AddressWithType LeAddressRotator::GetCurrentAddress() {
  ASSERT(address_policy_ != AddressPolicy::POLICY_NOT_SET);
  switch (address_policy_) {
    case AddressPolicy::USE_PUBLIC_ADDRESS:
      return public_address_;
    case AddressPolicy::USE_STATIC_ADDRESS:
      return fixed_address_;
    case AddressPolicy::USE_NON_RESOLVABLE_ADDRESS:
    case AddressPolicy::USE_RESOLVABLE_ADDRESS:
      return le_random_address_;
    default:
      LOG_ALWAYS_FATAL("invalid parameters");
  }
}

AddressWithType LeAddressRotator::GetAnotherAddress() {
  std::array<uint8_t, 8> random = os::GenerateRandom<8>();
  hci::Address address = generate_rpa(rotation_irk_, random);
  auto random_address = AddressWithType(address, AddressType::RANDOM_DEVICE_ADDRESS);
  return random_address;
}

void LeAddressRotator::SetAddress(AddressWithType address_with_type) {
  use_address_from_set_address = true;
  le_random_address_ = address_with_type;
}

void LeAddressRotator::pause_registered_clients() {
  for (auto client : registered_clients_) {
    if (client.second != ClientState::PAUSED && client.second != ClientState::WAITING_FOR_PAUSE) {
      client.second = ClientState::WAITING_FOR_PAUSE;
      client.first->OnPause();
    }
  }
}

void LeAddressRotator::ack_pause(LeAddressRotatorCallback* callback) {
  ASSERT(registered_clients_.find(callback) != registered_clients_.end());
  registered_clients_.find(callback)->second = ClientState::PAUSED;
  for (auto client : registered_clients_) {
    if (client.second != ClientState::PAUSED) {
      // make sure all client paused
      return;
    }
  }
  rotate_random_address();
}

void LeAddressRotator::resume_registered_clients() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto client : registered_clients_) {
    client.second = ClientState::WAITING_FOR_RESUME;
    client.first->OnResume();
  }
}

void LeAddressRotator::ack_resume(LeAddressRotatorCallback* callback) {
  ASSERT(registered_clients_.find(callback) != registered_clients_.end());
  registered_clients_.find(callback)->second = ClientState::RESUMED;
}

void LeAddressRotator::rotate_random_address() {
  if (address_policy_ != AddressPolicy::USE_RESOLVABLE_ADDRESS &&
      address_policy_ != AddressPolicy::USE_NON_RESOLVABLE_ADDRESS) {
    return;
  }

  address_rotation_alarm_->Schedule(
      common::BindOnce(&LeAddressRotator::pause_registered_clients, common::Unretained(this)),
      get_next_private_address_interval_ms());

  if (use_address_from_set_address) {
    use_address_from_set_address = false;
    handler_->Post(common::Bind(set_random_address_, le_random_address_.GetAddress()));
    return;
  }

  hci::Address address;
  if (address_policy_ == AddressPolicy::USE_RESOLVABLE_ADDRESS) {
    std::array<uint8_t, 8> random = os::GenerateRandom<8>();
    address = generate_rpa(rotation_irk_, random);
  } else {
    address = generate_nrpa();
  }
  handler_->Post(common::Bind(set_random_address_, address));
  le_random_address_ = AddressWithType(address, AddressType::RANDOM_DEVICE_ADDRESS);
}

/* This function generates Resolvable Private Address (RPA) from Identity
 * Resolving Key |irk| and |prand|*/
hci::Address LeAddressRotator::generate_rpa(const crypto_toolbox::Octet16& irk, std::array<uint8_t, 8> prand) {
  // most significant bit, bit7, bit6 is 01 to be resolvable random
  // Bits of the random part of prand shall not be all 1 or all 0
  constexpr uint8_t BLE_RESOLVE_ADDR_MSB = 0x40;
  prand[2] &= (~BLE_ADDR_MASK);
  if ((prand[0] == 0x00 && prand[1] == 0x00 && prand[2] == 0x00) ||
      (prand[0] == 0xFF && prand[1] == 0xFF && prand[2] == 0x3F)) {
    prand[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }
  prand[2] |= BLE_RESOLVE_ADDR_MSB;

  hci::Address address;
  address.address[3] = prand[0];
  address.address[4] = prand[1];
  address.address[5] = prand[2];

  /* encrypt with IRK */
  crypto_toolbox::Octet16 p = crypto_toolbox::aes_128(irk, prand.data(), 3);

  /* set hash to be LSB of rpAddress */
  address.address[0] = p[0];
  address.address[1] = p[1];
  address.address[2] = p[2];
  return address;
}

// This function generates NON-Resolvable Private Address (NRPA)
hci::Address LeAddressRotator::generate_nrpa() {
  // The two most significant bits of the address shall be equal to 0
  // Bits of the random part of the address shall not be all 1 or all 0
  std::array<uint8_t, 6> random = os::GenerateRandom<6>();
  random[5] &= (~BLE_ADDR_MASK);
  if ((random[0] == 0x00 && random[1] == 0x00 && random[2] == 0x00 && random[3] == 0x00 && random[4] == 0x00 &&
       random[5] == 0x00) ||
      (random[0] == 0xFF && random[1] == 0xFF && random[2] == 0xFF && random[3] == 0xFF && random[4] == 0xFF &&
       random[5] == 0x3F)) {
    random[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }

  hci::Address address;
  address.address[0] = random[0];
  address.address[1] = random[1];
  address.address[2] = random[2];
  address.address[3] = random[3];
  address.address[4] = random[4];
  address.address[5] = random[5];

  // the address shall not be equal to the public address
  while (address == public_address_.GetAddress()) {
    address.address[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }

  return address;
}

std::chrono::milliseconds LeAddressRotator::get_next_private_address_interval_ms() {
  auto interval_random_part_max_ms = maximum_rotation_time_ - minimum_rotation_time_;
  auto random_ms = std::chrono::milliseconds(os::GenerateRandom()) % (interval_random_part_max_ms);
  return minimum_rotation_time_ + random_ms;
}

}  // namespace hci
}  // namespace bluetooth
