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
// Media Sinks
// Implementation

#include "MediaSink.hh"
#include "GroupsockHelper.hh"
#include <string.h>

////////// MediaSink //////////

MediaSink::MediaSink(UsageEnvironment& env)
  : Medium(env), fSource(NULL) {
}

MediaSink::~MediaSink() {
  stopPlaying();
}

Boolean MediaSink::isSink() const {
  return True;
}

Boolean MediaSink::lookupByName(UsageEnvironment& env, char const* sinkName,
				MediaSink*& resultSink) {
  resultSink = NULL; // unless we succeed

  Medium* medium;
  if (!Medium::lookupByName(env, sinkName, medium)) return False;

  if (!medium->isSink()) {
    env.setResultMsg(sinkName, " is not a media sink");
    return False;
  }

  resultSink = (MediaSink*)medium;
  return True;
}

Boolean MediaSink::sourceIsCompatibleWithUs(MediaSource& source) {
  // We currently support only framed sources.
  return source.isFramedSource();
}

Boolean MediaSink::startPlaying(MediaSource& source,
				afterPlayingFunc* afterFunc,
				void* afterClientData) {
  // Make sure we're not already being played:
  if (fSource != NULL) {
    envir().setResultMsg("This sink is already being played");
    return False;
  }

  // Make sure our source is compatible:
  if (!sourceIsCompatibleWithUs(source)) {
    envir().setResultMsg("MediaSink::startPlaying(): source is not compatible!");
    return False;
  }
  fSource = (FramedSource*)&source;

  fAfterFunc = afterFunc;
  fAfterClientData = afterClientData;
  return continuePlaying();
}

void MediaSink::stopPlaying() {
  // First, tell the source that we're no longer interested:
  if (fSource != NULL) fSource->stopGettingFrames();

  // Cancel any pending tasks:
  envir().taskScheduler().unscheduleDelayedTask(nextTask());

  fSource = NULL; // indicates that we can be played again
  fAfterFunc = NULL;
}

void MediaSink::onSourceClosure(void* clientData) {
  MediaSink* sink = (MediaSink*)clientData;
  sink->onSourceClosure();
}

void MediaSink::onSourceClosure() {
  // Cancel any pending tasks:
  envir().taskScheduler().unscheduleDelayedTask(nextTask());

  fSource = NULL; // indicates that we can be played again
  if (fAfterFunc != NULL) {
    (*fAfterFunc)(fAfterClientData);
  }
}

Boolean MediaSink::isRTPSink() const {
  return False; // default implementation
}

////////// OutPacketBuffer //////////

unsigned OutPacketBuffer::maxSize = 2000000; // by default

OutPacketBuffer
::OutPacketBuffer(unsigned preferredPacketSize, unsigned maxPacketSize, unsigned maxBufferSize)
  : fPreferred(preferredPacketSize), fMax(maxPacketSize),
    fOverflowDataSize(0) {
  if (maxBufferSize == 0) maxBufferSize = maxSize;
  unsigned maxNumPackets = (maxBufferSize + (maxPacketSize-1))/maxPacketSize;
  fLimit = maxNumPackets*maxPacketSize;
  fBuf = new unsigned char[fLimit];
  resetPacketStart();
  resetOffset();
  resetOverflowData();
}

OutPacketBuffer::~OutPacketBuffer() {
  delete[] fBuf;
}

// 向输出缓冲末尾追加写入数据
void OutPacketBuffer::enqueue(unsigned char const* from, unsigned numBytes) {
  if (numBytes > totalBytesAvailable()) {
#ifdef DEBUG
    fprintf(stderr, "OutPacketBuffer::enqueue() warning: %d > %d\n", numBytes, totalBytesAvailable());
#endif
    numBytes = totalBytesAvailable();
  }

  // 防止重复写入
  if (curPtr() != from) memmove(curPtr(), from, numBytes);
  increment(numBytes);
}

// 向输出缓冲末尾追加写入4B数据，且从主机字节序转为网络字节序
void OutPacketBuffer::enqueueWord(u_int32_t word) {
  u_int32_t nWord = htonl(word);
  enqueue((unsigned char*)&nWord, 4);
}

// 向输出缓冲指定位置覆盖插入数据，且更新输出缓冲有效字节数
void OutPacketBuffer::insert(unsigned char const* from, unsigned numBytes,
			     unsigned toPosition) {
  unsigned realToPosition = fPacketStart + toPosition;
  if (realToPosition + numBytes > fLimit) {
    if (realToPosition > fLimit) return; // we can't do this
    numBytes = fLimit - realToPosition;
  }

  memmove(&fBuf[realToPosition], from, numBytes);
  if (toPosition + numBytes > fCurOffset) {
    fCurOffset = toPosition + numBytes;
  }
}

// 覆盖插入，主机字节数转网络字节序
void OutPacketBuffer::insertWord(u_int32_t word, unsigned toPosition) {
  u_int32_t nWord = htonl(word);
  insert((unsigned char*)&nWord, 4, toPosition);
}

// 从输出缓存指定位置深度拷贝出内存，截断拷贝
void OutPacketBuffer::extract(unsigned char* to, unsigned numBytes,
			      unsigned fromPosition) {
  unsigned realFromPosition = fPacketStart + fromPosition;
  if (realFromPosition + numBytes > fLimit) { // sanity check
    if (realFromPosition > fLimit) return; // we can't do this
    numBytes = fLimit - realFromPosition;
  }

  memmove(to, &fBuf[realFromPosition], numBytes);
}
// 从输出缓存指定位置深度拷贝4B，且网络字节序转为主机字节序
u_int32_t OutPacketBuffer::extractWord(unsigned fromPosition) {
  u_int32_t nWord;
  extract((unsigned char*)&nWord, 4, fromPosition);
  return ntohl(nWord);
}

// 跳过指定字节数 
void OutPacketBuffer::skipBytes(unsigned numBytes) {
  if (numBytes > totalBytesAvailable()) {
    numBytes = totalBytesAvailable();
  }

  increment(numBytes);
}

void OutPacketBuffer
::setOverflowData(unsigned overflowDataOffset,
		  unsigned overflowDataSize,
		  struct timeval const& presentationTime,
		  unsigned durationInMicroseconds) {
  fOverflowDataOffset = overflowDataOffset;
  fOverflowDataSize = overflowDataSize;
  fOverflowPresentationTime = presentationTime;
  fOverflowDurationInMicroseconds = durationInMicroseconds;
}
//将溢出分片数据追加写入输出缓冲，一般用于源给的数据太多，需要分片多次发送的情况
void OutPacketBuffer::useOverflowData() {
  enqueue(&fBuf[fPacketStart + fOverflowDataOffset], fOverflowDataSize);
  fCurOffset -= fOverflowDataSize; // undoes increment performed by "enqueue"
  resetOverflowData();
}

// 输出有效数据缓冲索引向后跳过指定字节, 溢出数据量不变
void OutPacketBuffer::adjustPacketStart(unsigned numBytes) {
  fPacketStart += numBytes;
  if (fOverflowDataOffset >= numBytes) {
    fOverflowDataOffset -= numBytes;
  } else {
    fOverflowDataOffset = 0;
    fOverflowDataSize = 0; // an error otherwise
  }
}

// 输出有效数据缓冲索引向前跳过指定字节, 溢出数据量不变
void OutPacketBuffer::resetPacketStart() {
  if (fOverflowDataSize > 0) {
    fOverflowDataOffset += fPacketStart;
  }
  fPacketStart = 0;
}
