// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file CruDmaChannel.cxx
/// \brief Implementation of the CruDmaChannel class.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)
/// \author Kostas Alexopoulos (kostas.alexopoulos@cern.ch)

#include <thread>
#include <boost/format.hpp>
#include "CruDmaChannel.h"
#include "ExceptionInternal.h"
#include "ReadoutCard/ChannelFactory.h"

using namespace std::literals;
using boost::format;

namespace AliceO2
{
namespace roc
{

CruDmaChannel::CruDmaChannel(const Parameters& parameters)
    : DmaChannelPdaBase(parameters, allowedChannels()), //
      mInitialResetLevel(ResetLevel::Internal), // It's good to reset at least the card channel in general
      mLoopbackMode(parameters.getGeneratorLoopback().get_value_or(LoopbackMode::Internal)), // DG loopback mode by default
      mGeneratorEnabled(parameters.getGeneratorEnabled().get_value_or(true)), // Use data generator by default
      mGeneratorPattern(parameters.getGeneratorPattern().get_value_or(GeneratorPattern::Incremental)), //
      mGeneratorDataSizeRandomEnabled(parameters.getGeneratorRandomSizeEnabled().get_value_or(false)), //
      mGeneratorMaximumEvents(0), // Infinite events
      mGeneratorInitialValue(0), // Start from 0
      mGeneratorInitialWord(0), // First word
      mGeneratorSeed(0), // Presumably for random patterns, incremental doesn't really need it
      mGeneratorDataSize(parameters.getGeneratorDataSize().get_value_or(Cru::DMA_PAGE_SIZE)), // Can use page size
      mDmaPageSize(parameters.getDmaPageSize().get_value_or(Cru::DMA_PAGE_SIZE))
{
  
  if (auto pageSize = parameters.getDmaPageSize()) {
    if (pageSize.get() != Cru::DMA_PAGE_SIZE) {
      log("DMA page size not default; Behaviour undefined", InfoLogger::InfoLogger::Warning);
      /*BOOST_THROW_EXCEPTION(CruException()
          << ErrorInfo::Message("CRU only supports an 8KiB page size")
          << ErrorInfo::DmaPageSize(pageSize.get()));*/
    }
  }

  if (mLoopbackMode == LoopbackMode::Diu || mLoopbackMode == LoopbackMode::Siu) { 
    BOOST_THROW_EXCEPTION(CruException() << ErrorInfo::Message("CRU does not support given loopback mode")
      << ErrorInfo::LoopbackMode(mLoopbackMode));
  }

  // Prep for BARs
  auto parameters2 = parameters;
  parameters2.setChannelNumber(2);
  auto bar = ChannelFactory().getBar(parameters);
  auto bar2 = ChannelFactory().getBar(parameters2);
  cruBar = std::move(std::dynamic_pointer_cast<CruBar> (bar)); // Initialize BAR 0
  cruBar2 = std::move(std::dynamic_pointer_cast<CruBar> (bar2)); // Initialize BAR 2
  mFeatures = getBar()->getFirmwareFeatures(); // Get which features of the firmware are enabled

  if (mFeatures.standalone) {
    std::stringstream stream;
    auto logFeature = [&](auto name, bool enabled) { if (!enabled) { stream << " " << name; }};
    stream << "Standalone firmware features disabled:";
    logFeature("firmware-info", mFeatures.firmwareInfo);
    logFeature("serial-number", mFeatures.serial);
    logFeature("temperature", mFeatures.temperature);
    logFeature("data-selection", mFeatures.dataSelection);
    log(stream.str());
  }

  // Insert links
  {
    std::stringstream stream;
    stream << "Enabling link(s): ";
    auto linkMask = parameters.getLinkMask().value_or(Parameters::LinkMaskType{0});
    mLinks.reserve(linkMask.size());
    for (uint32_t id : linkMask) {
      if (id >= Cru::MAX_LINKS) {
        BOOST_THROW_EXCEPTION(InvalidLinkId() << ErrorInfo::Message("CRU does not support given link ID")
          << ErrorInfo::LinkId(id));
      }
      stream << id << " ";
      mLinks.push_back({static_cast<LinkId>(id)});
    }
    log(stream.str());
  }

  std::stringstream stream;
  stream << "Generator enabled: " << mGeneratorEnabled << " | Loopback mode: " << LoopbackMode::toString(mLoopbackMode);
  log(stream.str());
}

auto CruDmaChannel::allowedChannels() -> AllowedChannels {
  // We have only one DMA channel per CRU endpoint
  return {0};
}

CruDmaChannel::~CruDmaChannel()
{
  setBufferNonReady();
  if (mReadyQueue.size() > 0) {
    log((format("Remaining superpages in the ready queue: %1%") % mReadyQueue.size()).str());
  }

  if (mLoopbackMode == LoopbackMode::Internal) {
    resetDebugMode();
  }

}

void CruDmaChannel::deviceStartDma()
{
  // Set data generator pattern
  if (mGeneratorEnabled) {
    getBar()->setDataGeneratorPattern(mGeneratorPattern, mGeneratorDataSize, mGeneratorDataSizeRandomEnabled);
  }

  // Set data source
  uint32_t dataSourceSelection = 0x0;
  if (mGeneratorEnabled) {
    if (mLoopbackMode == LoopbackMode::Internal) {
      enableDebugMode();
      dataSourceSelection = Cru::Registers::DATA_SOURCE_SELECT_INTERNAL;
    } else if (mLoopbackMode == LoopbackMode::Ddg) {
      dataSourceSelection = Cru::Registers::DATA_SOURCE_SELECT_GBT;
    } else {
      BOOST_THROW_EXCEPTION(CruException()
        << ErrorInfo::Message("CRU only support 'Internal' or 'Ddg' for data generator"));
    }
  } else {
    if (mLoopbackMode == LoopbackMode::None) {
      dataSourceSelection = Cru::Registers::DATA_SOURCE_SELECT_GBT;
    } else {
      BOOST_THROW_EXCEPTION(CruException()
        << ErrorInfo::Message("CRU only supports 'None' loopback mode when operating without a data generator"));
    }
  }

  if (mFeatures.dataSelection) {
    getBar()->setDataSource(dataSourceSelection);
  } else {
    log("Did not set data source, feature not supported by firmware", InfoLogger::InfoLogger::Warning);
  }

  // Reset CRU (should be done after link mask set)
  resetCru();

  // Initialize link queues
  for (auto &link : mLinks) {
    link.queue.clear();
    link.superpageCounter = 0;
  }
  mReadyQueue.clear();
  mLinkQueuesTotalAvailable = LINK_QUEUE_CAPACITY * mLinks.size();

  // Start DMA
  setBufferReady();

  // Enable data taking
  if (dataSourceSelection == Cru::Registers::DATA_SOURCE_SELECT_GBT) {
    getBar2()->disableDataTaking(); // Make sure we don't start from a bad state
    getBar2()->enableDataTaking();
  }
}

/// Set buffer to ready
void CruDmaChannel::setBufferReady()
{
  getBar()->setDataEmulatorEnabled(true);
  std::this_thread::sleep_for(10ms);
}

/// Set buffer to non-ready
void CruDmaChannel::setBufferNonReady()
{
  getBar()->setDataEmulatorEnabled(false);
}

void CruDmaChannel::deviceStopDma()
{
  setBufferNonReady();
  getBar2()->disableDataTaking();
  int moved = 0;
  for (auto& link : mLinks) {
    int32_t superpageCount = getBar()->getSuperpageCount(link.id);
    uint32_t amountAvailable = superpageCount - link.superpageCounter;
    //log((format("superpageCount %1% amountAvailable %2%") % superpageCount % amountAvailable).str());
    for (uint32_t i = 0; i < (amountAvailable + 1); ++i) { // get an extra, possibly partly filled superpage
      if (mReadyQueue.size() >= READY_QUEUE_CAPACITY) {
        break;
      }
      if (!link.queue.empty()) { // care for the extra filled superpage
        transferSuperpageFromLinkToReady(link);
        moved++;
      }
    }
    assert(link.queue.empty());
  }
  assert(mLinkQueuesTotalAvailable == LINK_QUEUE_CAPACITY * mLinks.size());
  log((format("Moved %1% remaining superpage(s) to ready queue") % moved).str());
}

void CruDmaChannel::deviceResetChannel(ResetLevel::type resetLevel)
{
  if (resetLevel == ResetLevel::Nothing) {
    return;
  }
  resetCru();
}

CardType::type CruDmaChannel::getCardType()
{
  return CardType::Cru;
}

void CruDmaChannel::resetCru()
{
  getBar()->resetDataGeneratorCounter();
  std::this_thread::sleep_for(100ms);
  getBar()->resetCard();
  std::this_thread::sleep_for(100ms);
}

auto CruDmaChannel::getNextLinkIndex() -> LinkIndex
{
  auto smallestQueueIndex = std::numeric_limits<LinkIndex>::max();
  auto smallestQueueSize = std::numeric_limits<size_t>::max();

  for (size_t i = 0; i < mLinks.size(); ++i) {
    auto queueSize = mLinks[i].queue.size();
    if (queueSize < smallestQueueSize) {
      smallestQueueIndex = i;
      smallestQueueSize = queueSize;
    }
  }

  return smallestQueueIndex;
}

void CruDmaChannel::pushSuperpage(Superpage superpage)
{
  checkSuperpage(superpage);

  if (mLinkQueuesTotalAvailable == 0) {
    // Note: the transfer queue refers to the firmware, not the mLinkIndexQueue which contains the LinkIds for links
    // that can still be pushed into (essentially the opposite of the firmware's queue).
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not push superpage, transfer queue was full"));
  }

  // Get the next link to push
  auto &link = mLinks[getNextLinkIndex()];

  if (link.queue.size() >= LINK_QUEUE_CAPACITY) {
    // Is the link's FIFO out of space?
    // This should never happen
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not push superpage, link queue was full"));
  }

  // Once we've confirmed the link has a slot available, we push the superpage
  pushSuperpageToLink(link, superpage);
  auto dmaPages = superpage.getSize() / mDmaPageSize;
  auto busAddress = getBusOffsetAddress(superpage.getOffset());
  getBar()->pushSuperpageDescriptor(link.id, dmaPages, busAddress);
}

auto CruDmaChannel::getSuperpage() -> Superpage
{
  if (mReadyQueue.empty()) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not get superpage, ready queue was empty"));
  }
  return mReadyQueue.front();
}

auto CruDmaChannel::popSuperpage() -> Superpage
{
  if (mReadyQueue.empty()) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not pop superpage, ready queue was empty"));
  }
  auto superpage = mReadyQueue.front();
  mReadyQueue.pop_front();
  return superpage;
}

void CruDmaChannel::pushSuperpageToLink(Link& link, const Superpage& superpage)
{
  mLinkQueuesTotalAvailable--;
  link.queue.push_back(superpage);
}

void CruDmaChannel::transferSuperpageFromLinkToReady(Link& link)
{
  if (link.queue.empty()) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not transfer Superpage from link to ready queue, link queue is empty"));
  }

  link.queue.front().setReady(true);
  link.queue.front().setReceived(link.queue.front().getSize());
  mReadyQueue.push_back(link.queue.front());
  link.queue.pop_front();
  link.superpageCounter++;
  mLinkQueuesTotalAvailable++;
}

void CruDmaChannel::fillSuperpages()
{
  // Check for arrivals & handle them
  const auto size = mLinks.size();
  for (LinkIndex linkIndex = 0; linkIndex < size; ++linkIndex) {
    auto& link = mLinks[linkIndex];
    uint32_t superpageCount = getBar()->getSuperpageCount(link.id);
    auto available = superpageCount > link.superpageCounter;
    if (available) {
      uint32_t amountAvailable = superpageCount - link.superpageCounter;
      if (amountAvailable > link.queue.size()) {
        std::stringstream stream;
        stream << "FATAL: Firmware reported more superpages available (" << amountAvailable <<
          ") than should be present in FIFO (" << link.queue.size() << "); "
          << link.superpageCounter << " superpages received from link " << int(link.id) << " according to driver, "
          << superpageCount << " pushed according to firmware";
        log(stream.str(), InfoLogger::InfoLogger::Error);
        BOOST_THROW_EXCEPTION(Exception()
            << ErrorInfo::Message("FATAL: Firmware reported more superpages available than should be present in FIFO"));
      }

      for (uint32_t i = 0; i < amountAvailable; ++i) {
        if (mReadyQueue.size() >= READY_QUEUE_CAPACITY) {
          break;
        }

        // Front superpage has arrived
        transferSuperpageFromLinkToReady(link);
      }
    }
  }
}

int CruDmaChannel::getTransferQueueAvailable()
{
  return mLinkQueuesTotalAvailable;
}

// Return a boolean that denotes whether the transfer queue is empty
// The transfer queue is empty when all its slots are available
bool CruDmaChannel::isTransferQueueEmpty()
{
  return mLinkQueuesTotalAvailable == (LINK_QUEUE_CAPACITY * mLinks.size());
}

int CruDmaChannel::getReadyQueueSize()
{
  return mReadyQueue.size();
}

// Return a boolean that denotes whether the ready queue is full
// The ready queue is full when the CRU has filled it up
bool CruDmaChannel::isReadyQueueFull()
{
  return mReadyQueue.size() == READY_QUEUE_CAPACITY;
}

int32_t CruDmaChannel::getDroppedPackets()
{
  return getBar2()->getDroppedPackets();
}

bool CruDmaChannel::injectError()
{
  if (mGeneratorEnabled) {
    getBar()->dataGeneratorInjectError();
    return true;
  } else {
    return false;
  }
}

void CruDmaChannel::enableDebugMode() {
  if(!getBar()->getDebugModeEnabled()) {
    getBar()->setDebugModeEnabled(true);
    mDebugRegisterReset = true;
  }
}

void CruDmaChannel::resetDebugMode() {
  if (mDebugRegisterReset) {
    getBar()->setDebugModeEnabled(false);
  }
}

boost::optional<int32_t> CruDmaChannel::getSerial()
{
  if (mFeatures.serial) {
    return getBar2()->getSerial();
  } else {
    return {};
  }
}

boost::optional<float> CruDmaChannel::getTemperature()
{
  if (mFeatures.temperature){
    return getBar2()->getTemperature();
  } else {
    return {};
  }
}

boost::optional<std::string> CruDmaChannel::getFirmwareInfo()
{
  if (mFeatures.firmwareInfo) {
    return getBar2()->getFirmwareInfo();
  } else {
    return {};
  }
}

boost::optional<std::string> CruDmaChannel::getCardId()
{
  if (mFeatures.chipId) {
    return getBar2()->getCardId();
  } else  {
    return {};
  }
}

} // namespace roc
} // namespace AliceO2
