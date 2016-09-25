/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// "liveMedia"
// Copyright (c) 1996-2016 Live Networks, Inc.  All rights reserved.
// RTP sink for a common kind of payload format: Those which pack multiple,
// complete codec frames (as many as possible) into each RTP packet.
// Implementation

#include "MultiFramedRTPSink.hh"
#include "GroupsockHelper.hh"

////////// MultiFramedRTPSink //////////

void MultiFramedRTPSink::setPacketSizes(unsigned preferredPacketSize,
					unsigned maxPacketSize) {
  if (preferredPacketSize > maxPacketSize || preferredPacketSize == 0) return;
      // sanity check

  delete fOutBuf;
  fOutBuf = new OutPacketBuffer(preferredPacketSize, maxPacketSize);
  fOurMaxPacketSize = maxPacketSize; // save value, in case subclasses need it
}

#ifndef RTP_PAYLOAD_MAX_SIZE
#define RTP_PAYLOAD_MAX_SIZE 1456
      // Default max packet size (1500, minus allowance for IP, UDP, UMTP headers)
      // (Also, make it a multiple of 4 bytes, just in case that matters.)
#endif
#ifndef RTP_PAYLOAD_PREFERRED_SIZE
#define RTP_PAYLOAD_PREFERRED_SIZE ((RTP_PAYLOAD_MAX_SIZE) < 1000 ? (RTP_PAYLOAD_MAX_SIZE) : 1000)
#endif

MultiFramedRTPSink::MultiFramedRTPSink(UsageEnvironment& env,
				       Groupsock* rtpGS,
				       unsigned char rtpPayloadType,
				       unsigned rtpTimestampFrequency,
				       char const* rtpPayloadFormatName,
				       unsigned numChannels)
  : RTPSink(env, rtpGS, rtpPayloadType, rtpTimestampFrequency,
	    rtpPayloadFormatName, numChannels),
    fOutBuf(NULL), fCurFragmentationOffset(0), fPreviousFrameEndedFragmentation(False),
    fOnSendErrorFunc(NULL), fOnSendErrorData(NULL) {
  setPacketSizes((RTP_PAYLOAD_PREFERRED_SIZE), (RTP_PAYLOAD_MAX_SIZE));
}

MultiFramedRTPSink::~MultiFramedRTPSink() {
  delete fOutBuf;
}

void MultiFramedRTPSink
::doSpecialFrameHandling(unsigned /*fragmentationOffset*/,
			 unsigned char* /*frameStart*/,
			 unsigned /*numBytesInFrame*/,
			 struct timeval framePresentationTime,
			 unsigned /*numRemainingBytes*/) {
  // default implementation: If this is the first frame in the packet,
  // use its presentationTime for the RTP timestamp:
  if (isFirstFrameInPacket()) {
    setTimestamp(framePresentationTime);
  }
}

Boolean MultiFramedRTPSink::allowFragmentationAfterStart() const {
  return False; // by default
}

Boolean MultiFramedRTPSink::allowOtherFramesAfterLastFragment() const {
  return False; // by default
}

Boolean MultiFramedRTPSink
::frameCanAppearAfterPacketStart(unsigned char const* /*frameStart*/,
				 unsigned /*numBytesInFrame*/) const {
  return True; // by default
}

unsigned MultiFramedRTPSink::specialHeaderSize() const {
  // default implementation: Assume no special header:
  return 0;
}

unsigned MultiFramedRTPSink::frameSpecificHeaderSize() const {
  // default implementation: Assume no frame-specific header:
  return 0;
}

unsigned MultiFramedRTPSink::computeOverflowForNewFrame(unsigned newFrameSize) const {
  // default implementation: Just call numOverflowBytes()
  return fOutBuf->numOverflowBytes(newFrameSize);
}

void MultiFramedRTPSink::setMarkerBit() {
  unsigned rtpHdr = fOutBuf->extractWord(0);
  rtpHdr |= 0x00800000;
  fOutBuf->insertWord(rtpHdr, 0);
}

// 当前系统时间相对timeBase增量转为RTP TimeStamp
void MultiFramedRTPSink::setTimestamp(struct timeval framePresentationTime) {
  // First, convert the presentation time to a 32-bit RTP timestamp:
  fCurrentTimestamp = convertToRTPTimestamp(framePresentationTime);

  // Then, insert it into the RTP packet:
  fOutBuf->insertWord(fCurrentTimestamp, fTimestampPosition);
}

void MultiFramedRTPSink::setSpecialHeaderWord(unsigned word,
					      unsigned wordPosition) {
  fOutBuf->insertWord(word, fSpecialHeaderPosition + 4*wordPosition);
}

void MultiFramedRTPSink::setSpecialHeaderBytes(unsigned char const* bytes,
					       unsigned numBytes,
					       unsigned bytePosition) {
  fOutBuf->insert(bytes, numBytes, fSpecialHeaderPosition + bytePosition);
}

void MultiFramedRTPSink::setFrameSpecificHeaderWord(unsigned word,
						    unsigned wordPosition) {
  fOutBuf->insertWord(word, fCurFrameSpecificHeaderPosition + 4*wordPosition);
}

void MultiFramedRTPSink::setFrameSpecificHeaderBytes(unsigned char const* bytes,
						     unsigned numBytes,
						     unsigned bytePosition) {
  fOutBuf->insert(bytes, numBytes, fCurFrameSpecificHeaderPosition + bytePosition);
}

// 向输出缓冲中追加写入填充数据，且最后一个字节为填充字节数
void MultiFramedRTPSink::setFramePadding(unsigned numPaddingBytes) {
  if (numPaddingBytes > 0) {
    // Add the padding bytes (with the last one being the padding size):
    unsigned char paddingBuffer[255]; //max padding
    memset(paddingBuffer, 0, numPaddingBytes);
    paddingBuffer[numPaddingBytes-1] = numPaddingBytes;
    fOutBuf->enqueue(paddingBuffer, numPaddingBytes);

    // Set the RTP padding bit:
    // 置RTP报头中的填充标志位
    unsigned rtpHdr = fOutBuf->extractWord(0);
    rtpHdr |= 0x20000000;
    fOutBuf->insertWord(rtpHdr, 0);
  }
}

// 开始从MediaSource获取帧数据并输出
Boolean MultiFramedRTPSink::continuePlaying() {
  // Send the first packet.
  // (This will also schedule any future sends.)
  buildAndSendPacket(True);
  return True;
}

// 停止获取数据并解除与MediaSource的绑定
void MultiFramedRTPSink::stopPlaying() {
  fOutBuf->resetPacketStart();
  fOutBuf->resetOffset();
  fOutBuf->resetOverflowData();

  // Then call the default "stopPlaying()" function:
  MediaSink::stopPlaying();
}

// 组建一个完整的RTP数据报
void MultiFramedRTPSink::buildAndSendPacket(Boolean isFirstPacket) {
  nextTask() = NULL;
  fIsFirstPacket = isFirstPacket;

  // Set up the RTP header:
  // 设置RTP数据头
  unsigned rtpHdr = 0x80000000; // RTP version 2; marker ('M') bit not set (by default; it can be set later)
  rtpHdr |= (fRTPPayloadType<<16);
  rtpHdr |= fSeqNo; // sequence number
  fOutBuf->enqueueWord(rtpHdr);

  // Note where the RTP timestamp will go.
  // (We can't fill this in until we start packing payload frames.)
  fTimestampPosition = fOutBuf->curPacketSize();
  fOutBuf->skipBytes(4); // leave a hole for the timestamp

  fOutBuf->enqueueWord(SSRC());

  // Allow for a special, payload-format-specific header following the
  // RTP header:
  fSpecialHeaderPosition = fOutBuf->curPacketSize();
  fSpecialHeaderSize = specialHeaderSize();
  fOutBuf->skipBytes(fSpecialHeaderSize);

  // Begin packing as many (complete) frames into the packet as we can:
  fTotalFrameSpecificHeaderSizes = 0;
  fNoFramesLeft = False;
  fNumFramesUsedSoFar = 0;
  packFrame();
}

// RTP数据负载迭代生成
void MultiFramedRTPSink::packFrame() {
  // Get the next frame.

  // First, skip over the space we'll use for any frame-specific header:
  // 跳过数据私有帧头,如sps,pps等
  fCurFrameSpecificHeaderPosition = fOutBuf->curPacketSize();
  fCurFrameSpecificHeaderSize = frameSpecificHeaderSize();
  fOutBuf->skipBytes(fCurFrameSpecificHeaderSize);
  fTotalFrameSpecificHeaderSizes += fCurFrameSpecificHeaderSize;

  // See if we have an overflow frame that was too big for the last pkt
  // 输出缓冲中还有未发送的分片帧数据，从输出缓冲中读取新的数据帧，继续发送; 否则，从MediaSource中读取新负载数据帧
  if (fOutBuf->haveOverflowData()) {
    // Use this frame before reading a new one from the source
    unsigned frameSize = fOutBuf->overflowDataSize();
    struct timeval presentationTime = fOutBuf->overflowPresentationTime();
    unsigned durationInMicroseconds = fOutBuf->overflowDurationInMicroseconds();
    fOutBuf->useOverflowData();

    afterGettingFrame1(frameSize, 0, presentationTime, durationInMicroseconds);
  } else {
    // Normal case: we need to read a new frame from the source
    if (fSource == NULL) return;
    fSource->getNextFrame(fOutBuf->curPtr(), fOutBuf->totalBytesAvailable(),
			  afterGettingFrame, this, ourHandleClosure, this);
  }
}

void MultiFramedRTPSink
::afterGettingFrame(void* clientData, unsigned numBytesRead,
		    unsigned numTruncatedBytes,
		    struct timeval presentationTime,
		    unsigned durationInMicroseconds) {
  MultiFramedRTPSink* sink = (MultiFramedRTPSink*)clientData;
  sink->afterGettingFrame1(numBytesRead, numTruncatedBytes,
			   presentationTime, durationInMicroseconds);
}

// numTruncatedBytes 输出缓冲中尾部截断丢弃的数据长度
// MediaSource的是按完整帧来返回数据的，所以超过输出可写缓冲的数据会被截断
// afterGettingFrame1的复杂之处在于处理frame的分片，
// 若一个frame大于TCP/UDP有效载荷(程序中定义为1448个字节)，就必需分片了。
// 最简单的情况就是一个packet(RTP包)中最多只充许一个frame，
// 即一个RTP包中存在一个frame或者frame的一个分片，H264就是这样处理的。
// 方法是将剩余的数据记录为buffer的溢出部分。
// 下次调用packFrame函数时，直接从溢出部分复制到packet中。
// 不过应该注意，一个frame的大小不能超过buffer的大小(默认为60000)，否则会真的溢出, 那就应该考虑增加buffer大小了。
void MultiFramedRTPSink
::afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes,
		     struct timeval presentationTime,
		     unsigned durationInMicroseconds) {
  // 更新时间戳
  if (fIsFirstPacket) {
    // Record the fact that we're starting to play now:
    // 第一个packet，则记录下当前时间 
    gettimeofday(&fNextSendTime, NULL);
  }

  // 更新pts
  fMostRecentPresentationTime = presentationTime;
  if (fInitialPresentationTime.tv_sec == 0 && fInitialPresentationTime.tv_usec == 0) {
    fInitialPresentationTime = presentationTime;
  }    

  // 截断警告
  //这里的处理要注意了，当一个Frame大于OutPacketBuffer::maxSize(默认值为60000)时，则会丢弃剩下的部分，numTruncatedBytes即为超出部分的大小。   
  //如果给予一帧的缓冲不够大，就会发生截断一帧数据的现象。但也只能提示一下用户 
  if (numTruncatedBytes > 0) {
    unsigned const bufferSize = fOutBuf->totalBytesAvailable();
    envir() << "MultiFramedRTPSink::afterGettingFrame1(): The input frame data was too large for our buffer size ("
	    << bufferSize << ").  "
	    << numTruncatedBytes << " bytes of trailing data was dropped!  Correct this by increasing \"OutPacketBuffer::maxSize\" to at least "
	    << OutPacketBuffer::maxSize + numTruncatedBytes << ", *before* creating this 'RTPSink'.  (Current value is "
	    << OutPacketBuffer::maxSize << ".)\n";
  }
  unsigned curFragmentationOffset = fCurFragmentationOffset;
  unsigned numFrameBytesToUse = frameSize;// 当前帧加入此次RTP进行发送的大小
  unsigned overflowBytes = 0;//当前帧中需缓存下次发送的数据

  // 处理当前rtp中已经有完整帧的情况
  // 初步决定是否缓存，即此次帧是否录入发送，看上一帧是否允许追加发送，没有上一帧，则自然视为允许
  // If we have already packed one or more frames into this packet,
  // check whether this new frame is eligible to be packed after them.
  // (This is independent of whether the packet has enough room for this
  // new frame; that check comes later.)
  //如果包已经打入帧数据了，并且不能再向这个包中加数据了，则把新获得的帧数据保存下来。  
  if (fNumFramesUsedSoFar > 0) {// 如果该RTP已经包含了起码一帧的数据，则考虑是否允许多帧合并传输
    if ((fPreviousFrameEndedFragmentation
	 && !allowOtherFramesAfterLastFragment())//上一帧已经完整录入rtp数据包，且不允许多帧合并传输
	|| !frameCanAppearAfterPacketStart(fOutBuf->curPtr(), frameSize)) {//RTP包没有足够的空间发送当前待发送帧的数据
      // Save away this frame for next time:
      // 将当前帧存储分片缓存，此次不发送
      numFrameBytesToUse = 0;
      fOutBuf->setOverflowData(fOutBuf->curPacketSize(), frameSize,
			       presentationTime, durationInMicroseconds);
    }
  }
  //表示可以追加数据
  fPreviousFrameEndedFragmentation = False;

  // 处理此次有数据需要发送的情况，看空间是否允许一次发送
  // 计算获取的帧中有多少数据可以打到当前包中，剩下的数据就作为overflow数据保存下来
  if (numFrameBytesToUse > 0) {
    // Check whether this frame overflows the packet
    // 若输出缓冲可写空间不够，则进行分片
    if (fOutBuf->wouldOverflow(frameSize)) {
      // Don't use this frame now; instead, save it as overflow data, and
      // send it in the next packet instead.  However, if the frame is too
      // big to fit in a packet by itself, then we need to fragment it (and
      // use some of it in this packet, if the payload format permits this.)
      // 若frame将导致packet溢出，应该将其保存到packet的溢出数据中，在下一个packet中发送
      // 如果frame本身大于pakcet 的max size， 就需要对frame进行分片操作。不过需要调用allowFragmentationAfterStart 函数以确定是否充许分片
      //    如果该帧是RTP的第一帧，或者允许分片,则进行分片
      //    否则，直接全部缓存至溢出空间
      if (isTooBigForAPacket(frameSize)
          && (fNumFramesUsedSoFar == 0 || allowFragmentationAfterStart())) {
        // We need to fragment this frame, and use some of it now:
        overflowBytes = computeOverflowForNewFrame(frameSize);
        numFrameBytesToUse -= overflowBytes;
        fCurFragmentationOffset += numFrameBytesToUse;
      } else {
        // We don't use any of this frame now:
        overflowBytes = frameSize;
        numFrameBytesToUse = 0;
      }
      fOutBuf->setOverflowData(fOutBuf->curPacketSize() + numFrameBytesToUse,
			       overflowBytes, presentationTime, durationInMicroseconds);
    } else if (fCurFragmentationOffset > 0) {
      // 证明这是当前帧的最后一个分片
      // This is the last fragment of a frame that was fragmented over
      // more than one packet.  Do any special handling for this case:
      fCurFragmentationOffset = 0;
      fPreviousFrameEndedFragmentation = True;
    }
  }

  // 进入发送逻辑，将numFrameBytesToUse字节的数据全部发送出去
  if (numFrameBytesToUse == 0 && frameSize > 0) {
    // Send our packet now, because we have filled it up:
    // 如果包中有数据并且没有新数据了，则发送之。（这种情况好像很难发生啊！）  
    sendPacketIfNecessary();
  } else {
    // Use this frame in our outgoing packet:
    // 需要向包中打入数据，表示新帧录入
    unsigned char* frameStart = fOutBuf->curPtr();
    fOutBuf->increment(numFrameBytesToUse);
        // do this now, in case "doSpecialFrameHandling()" calls "setFramePadding()" to append padding bytes

    // Here's where any payload format specific processing gets done:
    // 还记得RTP头中预留的空间吗，将在这个函数中进行填充
    doSpecialFrameHandling(curFragmentationOffset, frameStart,
			   numFrameBytesToUse, presentationTime,
			   overflowBytes);
    // 一帧录入
    ++fNumFramesUsedSoFar;

    // Update the time at which the next packet should be sent, based
    // on the duration of the frame that we just packed into it.
    // However, if this frame has overflow data remaining, then don't
    // count its duration yet.
    // 设置下一个packet的时间信息
    // 这里若存在overflow数据，就不需要更新时间，因为这是同一个frame的不同分片，需要保证时间一致
    if (overflowBytes == 0) {
      fNextSendTime.tv_usec += durationInMicroseconds;
      fNextSendTime.tv_sec += fNextSendTime.tv_usec/1000000;
      fNextSendTime.tv_usec %= 1000000;
    }

    // Send our packet now if (i) it's already at our preferred size, or
    // (ii) (heuristic) another frame of the same size as the one we just
    //      read would overflow the packet, or
    // (iii) it contains the last fragment of a fragmented frame, and we
    //      don't allow anything else to follow this or
    // (iv) one frame per packet is allowed:
    // 如果需要，就发出包，否则继续打入数据。  
    // 输出缓冲数据达到理想发送大小
    // 输出缓冲数据已满，即已溢出
    // 
    if (fOutBuf->isPreferredSize()
        || fOutBuf->wouldOverflow(numFrameBytesToUse)
        || (fPreviousFrameEndedFragmentation &&
            !allowOtherFramesAfterLastFragment())
        || !frameCanAppearAfterPacketStart(fOutBuf->curPtr() - frameSize,
					   frameSize) ) {
      // The packet is ready to be sent now
      sendPacketIfNecessary();
    } else {
      // There's room for more frames; try getting another:
      //packet中还可以容纳frame，这里将形成递归调用 
      packFrame();
    }
  }
}

static unsigned const rtpHeaderSize = 12;

Boolean MultiFramedRTPSink::isTooBigForAPacket(unsigned numBytes) const {
  // Check whether a 'numBytes'-byte frame - together with a RTP header and
  // (possible) special headers - would be too big for an output packet:
  // (Later allow for RTP extension header!) #####
  numBytes += rtpHeaderSize + specialHeaderSize() + frameSpecificHeaderSize();
  return fOutBuf->isTooBigForAPacket(numBytes);
}

void MultiFramedRTPSink::sendPacketIfNecessary() {
  if (fNumFramesUsedSoFar > 0) {
    // Send the packet:
#ifdef TEST_LOSS
    if ((our_random()%10) != 0) // simulate 10% packet loss #####
#endif
      if (!fRTPInterface.sendPacket(fOutBuf->packet(), fOutBuf->curPacketSize())) {
	// if failure handler has been specified, call it
	if (fOnSendErrorFunc != NULL) (*fOnSendErrorFunc)(fOnSendErrorData);
      }
    ++fPacketCount;
    fTotalOctetCount += fOutBuf->curPacketSize();
    fOctetCount += fOutBuf->curPacketSize()
      - rtpHeaderSize - fSpecialHeaderSize - fTotalFrameSpecificHeaderSizes;

    ++fSeqNo; // for next time
  }

  if (fOutBuf->haveOverflowData()
      && fOutBuf->totalBytesAvailable() > fOutBuf->totalBufferSize()/2) {
    // Efficiency hack: Reset the packet start pointer to just in front of
    // the overflow data (allowing for the RTP header and special headers),
    // so that we probably don't have to "memmove()" the overflow data
    // into place when building the next packet:
    unsigned newPacketStart = fOutBuf->curPacketSize()
      - (rtpHeaderSize + fSpecialHeaderSize + frameSpecificHeaderSize());
    fOutBuf->adjustPacketStart(newPacketStart);
  } else {
    // Normal case: Reset the packet start pointer back to the start:
    fOutBuf->resetPacketStart();
  }
  fOutBuf->resetOffset();
  fNumFramesUsedSoFar = 0;

  if (fNoFramesLeft) {
    // We're done:
    onSourceClosure();
  } else {
    // We have more frames left to send.  Figure out when the next frame
    // is due to start playing, then make sure that we wait this long before
    // sending the next packet.
    struct timeval timeNow;
    gettimeofday(&timeNow, NULL);
    int secsDiff = fNextSendTime.tv_sec - timeNow.tv_sec;
    int64_t uSecondsToGo = secsDiff*1000000 + (fNextSendTime.tv_usec - timeNow.tv_usec);
    if (uSecondsToGo < 0 || secsDiff < 0) { // sanity check: Make sure that the time-to-delay is non-negative:
      uSecondsToGo = 0;
    }

    // Delay this amount of time:
    nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecondsToGo, (TaskFunc*)sendNext, this);
  }
}

// The following is called after each delay between packet sends:
void MultiFramedRTPSink::sendNext(void* firstArg) {
  MultiFramedRTPSink* sink = (MultiFramedRTPSink*)firstArg;
  sink->buildAndSendPacket(False);
}

void MultiFramedRTPSink::ourHandleClosure(void* clientData) {
  MultiFramedRTPSink* sink = (MultiFramedRTPSink*)clientData;
  // There are no frames left, but we may have a partially built packet
  //  to send
  sink->fNoFramesLeft = True;
  sink->sendPacketIfNecessary();
}
