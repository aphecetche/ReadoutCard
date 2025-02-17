// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file DummyDmaChannel.h
/// \brief Definition of the DummyDmaChannel class.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#ifndef ALICEO2_SRC_READOUTCARD_DUMMY_DUMMYDMACHANNEL_H_
#define ALICEO2_SRC_READOUTCARD_DUMMY_DUMMYDMACHANNEL_H_

#include <array>
#include <boost/scoped_ptr.hpp>
#include <boost/circular_buffer_fwd.hpp>
#include <boost/circular_buffer.hpp>
#include "DmaChannelBase.h"

namespace AliceO2 {
namespace roc {

/// A dummy implementation of the DmaChannelInterface.
/// This exists so that the ReadoutCard module may be built even if the all the dependencies of the 'real' card
/// implementation are not met (this mainly concerns the PDA driver library).
/// It provides some basic simulation of page pushing and output.
class DummyDmaChannel final : public DmaChannelBase
{
  public:

    DummyDmaChannel(const Parameters& parameters);
    virtual ~DummyDmaChannel();

    virtual void pushSuperpage(Superpage) override;
    virtual Superpage getSuperpage() override;
    virtual Superpage popSuperpage() override;
    virtual void fillSuperpages() override;
    virtual bool isTransferQueueEmpty() override;
    virtual bool isReadyQueueFull() override;
    virtual int32_t getDroppedPackets() override;

    virtual bool injectError() override
    {
      return false;
    }
    virtual boost::optional<int32_t> getSerial() override;
    virtual boost::optional<float> getTemperature() override;
    virtual boost::optional<std::string> getFirmwareInfo() override;
    virtual int getTransferQueueAvailable() override;
    virtual int getReadyQueueSize() override;
    virtual void resetChannel(ResetLevel::type resetLevel) override;
    virtual void startDma() override;
    virtual void stopDma() override;
    virtual CardType::type getCardType() override;
    virtual PciAddress getPciAddress() override;
    virtual int getNumaNode() override;

  private:
    using Queue = boost::circular_buffer<Superpage>;

    Queue mTransferQueue;
    Queue mReadyQueue;
    size_t mBufferSize;
};

} // namespace roc
} // namespace AliceO2

#endif // ALICEO2_SRC_READOUTCARD_DUMMY_DUMMYDMACHANNEL_H_
