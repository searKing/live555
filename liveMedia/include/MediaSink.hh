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
// C++ header

#ifndef _MEDIA_SINK_HH
#define _MEDIA_SINK_HH

#ifndef _FRAMED_SOURCE_HH
#include "FramedSource.hh"
#endif

class MediaSink: public Medium {
public:
  static Boolean lookupByName(UsageEnvironment& env, char const* sinkName,
			      MediaSink*& resultSink);

  typedef void (afterPlayingFunc)(void* clientData);
  Boolean startPlaying(MediaSource& source,
		       afterPlayingFunc* afterFunc,
		       void* afterClientData);
  virtual void stopPlaying();

  // Test for specific types of sink:
  virtual Boolean isRTPSink() const;

  FramedSource* source() const {return fSource;}

protected:
  MediaSink(UsageEnvironment& env); // abstract base class
  virtual ~MediaSink();

  virtual Boolean sourceIsCompatibleWithUs(MediaSource& source);
      // called by startPlaying()
  virtual Boolean continuePlaying() = 0;
      // called by startPlaying()

  static void onSourceClosure(void* clientData); // can be used in "getNextFrame()" calls
  void onSourceClosure();
      // should be called (on ourselves) by continuePlaying() when it
      // discovers that the source we're playing from has closed.

  FramedSource* fSource;

private:
  // redefined virtual functions:
  virtual Boolean isSink() const;

private:
  // The following fields are used when we're being played:
  afterPlayingFunc* fAfterFunc;
  void* fAfterClientData;
};

// A data structure that a sink may use for an output packet:
class OutPacketBuffer {
public:
  OutPacketBuffer(unsigned preferredPacketSize, unsigned maxPacketSize,
		  unsigned maxBufferSize = 0);
      // if "maxBufferSize" is >0, use it - instead of "maxSize" to compute the buffer size
  ~OutPacketBuffer();

  static unsigned maxSize;
  static void increaseMaxSizeTo(unsigned newMaxSize) { if (newMaxSize > OutPacketBuffer::maxSize) OutPacketBuffer::maxSize = newMaxSize; }

  // ��ǰ��������п�дָ��
  unsigned char* curPtr() const {return &fBuf[fPacketStart + fCurOffset];}
  unsigned totalBytesAvailable() const {
    return fLimit - (fPacketStart + fCurOffset);
  }
  unsigned totalBufferSize() const { return fLimit; }
  unsigned char* packet() const {return &fBuf[fPacketStart];}
  unsigned curPacketSize() const {return fCurOffset;}

  // �ֿմ���������ж�������
  void increment(unsigned numBytes) {fCurOffset += numBytes;}

  void enqueue(unsigned char const* from, unsigned numBytes);
  void enqueueWord(u_int32_t word);
  void insert(unsigned char const* from, unsigned numBytes, unsigned toPosition);
  void insertWord(u_int32_t word, unsigned toPosition);
  void extract(unsigned char* to, unsigned numBytes, unsigned fromPosition);
  u_int32_t extractWord(unsigned fromPosition);

  void skipBytes(unsigned numBytes);

  Boolean isPreferredSize() const {return fCurOffset >= fPreferred;}
  Boolean wouldOverflow(unsigned numBytes) const {
    return (fCurOffset+numBytes) > fMax;
  }
  //��ȡ��ǰ��ô���������д�룬��������ֽ���
  unsigned numOverflowBytes(unsigned numBytes) const {
    return (fCurOffset+numBytes) - fMax;
  }
  // numBytes:RTP����С
  Boolean isTooBigForAPacket(unsigned numBytes) const {
    return numBytes > fMax;
  }

  void setOverflowData(unsigned overflowDataOffset,
		       unsigned overflowDataSize,
		       struct timeval const& presentationTime,
		       unsigned durationInMicroseconds);
  unsigned overflowDataSize() const {return fOverflowDataSize;}
  struct timeval overflowPresentationTime() const {return fOverflowPresentationTime;}
  unsigned overflowDurationInMicroseconds() const {return fOverflowDurationInMicroseconds;}
  Boolean haveOverflowData() const {return fOverflowDataSize > 0;}
  void useOverflowData();

  void adjustPacketStart(unsigned numBytes);
  void resetPacketStart();
  void resetOffset() { fCurOffset = 0; }
  void resetOverflowData() { fOverflowDataOffset = fOverflowDataSize = 0; }

private:
  // fPacketStart   : ���������Ч�����׵�ַ
  // fCurOffset     : ��������е�ǰ��дλ�ã�����д�ֽ���
  // fPreferred     : �������ݱ��ĵ�����С
  // fMax           : ������յ�������ݱ��ĵ�����С
  // fLimit         : ������建�����ܴ�С = fMax*maxBufferSize
  unsigned fPacketStart, fCurOffset, fPreferred, fMax, fLimit;
  // fBuf           : ��������׵�ַ
  unsigned char* fBuf;

  // ����֡��Ƭ����֡�����趨��RTP���ݱ�������С�ǣ���Ҫ��Ƭ��������
  // fOverflowDataOffset    : ������������Ч���������fPacketStart��ƫ���׵�ַ
  //                        : Ҳ������������������׵�ַΪ: fBuf + fPacketStart + fOverflowDataOffset
  // fOverflowDataSize      : ��Ч������ݵĴ�С
  unsigned fOverflowDataOffset, fOverflowDataSize;
  // ͬһ֡�Ĳ�ͬ��Ƭ��ʱ������ֲ���
  struct timeval fOverflowPresentationTime;
  unsigned fOverflowDurationInMicroseconds;
};

#endif
