//
//  CameraStreamerTests.swift
//  TetherFrameworkTests
//

import CoreMedia
import Foundation
import Testing
@testable import TetherFramework

struct CameraStreamerTests {
    // A real 128x96 baseline SPS/PPS; CoreMedia parses them.
    private let sps: [UInt8] = [0x67, 0x42, 0x00, 0x0A, 0xF8, 0x41, 0xA2]
    private let pps: [UInt8] = [0x68, 0xCE, 0x38, 0x80]

    // VideoToolbox emits AVCC (4-byte lengths); the daemon's decoder wants Annex B.
    private func sample(avcc: [UInt8], keyframe: Bool) throws -> CMSampleBuffer {
        var format: CMFormatDescription?
        let status = sps.withUnsafeBufferPointer { s in
            pps.withUnsafeBufferPointer { p in
                let pointers = [s.baseAddress!, p.baseAddress!]
                let sizes = [sps.count, pps.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: nil, parameterSetCount: 2, parameterSetPointers: pointers,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &format)
            }
        }
        try #require(status == noErr)

        var block: CMBlockBuffer?
        try #require(CMBlockBufferCreateWithMemoryBlock(
            allocator: nil, memoryBlock: nil, blockLength: avcc.count, blockAllocator: nil,
            customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: 0,
            blockBufferOut: &block) == noErr)
        try #require(CMBlockBufferReplaceDataBytes(
            with: avcc, blockBuffer: block!, offsetIntoDestination: 0, dataLength: avcc.count) == noErr)

        var out: CMSampleBuffer?
        var size = avcc.count
        try #require(CMSampleBufferCreateReady(
            allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1,
            sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1,
            sampleSizeArray: &size, sampleBufferOut: &out) == noErr)
        let sample = try #require(out)

        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true)! as NSArray
        let dict = attachments[0] as! NSMutableDictionary
        dict[kCMSampleAttachmentKey_NotSync] = !keyframe
        return sample
    }

    @Test func keyframeGetsParameterSetsAndStartCodes() throws {
        let nal: [UInt8] = [0x65, 0xAA, 0xBB]
        let payload = try #require(CameraStreamer.annexB(try sample(avcc: [0, 0, 0, 3] + nal, keyframe: true)))
        let start: [UInt8] = [0, 0, 0, 1]
        #expect(Array(payload) == start + sps + start + pps + start + nal)
    }

    @Test func deltaFrameSplitsEveryNal() throws {
        let avcc: [UInt8] = [0, 0, 0, 2, 0x41, 0x01, 0, 0, 0, 1, 0x06]
        let payload = try #require(CameraStreamer.annexB(try sample(avcc: avcc, keyframe: false)))
        #expect(Array(payload) == [0, 0, 0, 1, 0x41, 0x01, 0, 0, 0, 1, 0x06])
    }

    @Test func truncatedNalIsRejected() throws {
        #expect(CameraStreamer.annexB(try sample(avcc: [0, 0, 0, 9, 0x41], keyframe: false)) == nil)
    }
}
