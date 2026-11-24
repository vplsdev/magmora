// The blocks around the head that are still pass-through stubs (XP0; the cabinet is cab/VirtualMic.h since XP6). Each later package replaces its stub's body and keeps
// the shape: prepare allocates, process works in place on the stereo pair and must leave the samples untouched
// (not even multiplied by one) while its block is off, so that the chain is the head bit for bit (gate G-X1).
#pragma once

namespace magmora::chain {

struct BlockStub {
  void prepare(double sampleRate, int maxBlockSize) {
    rate = sampleRate;
    maxBlock = maxBlockSize;
  }
  void reset() {}
  template <class Sample>
  void process(Sample*, Sample*, int) {}  // a stub passes everything through, on or off
  double rate = 0.0;
  int maxBlock = 0;
};

struct FxSlotStub : BlockStub {};  // reserved, empty in this round

}  // namespace magmora::chain
