//
//  FrameFitterTests.swift
//  TetherFrameworkTests
//

import CoreVideo
import Foundation
import Testing
@testable import TetherFramework

struct FrameFitterTests {
    private func makeFrame(width: Int, height: Int, luma: UInt8) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let attributes = [kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()] as CFDictionary
        try #require(CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                         attributes, &buffer) == kCVReturnSuccess)
        let frame = try #require(buffer)
        fill(frame, luma: luma)
        return frame
    }

    private func fill(_ frame: CVPixelBuffer, luma: UInt8) {
        CVPixelBufferLockBaseAddress(frame, [])
        defer { CVPixelBufferUnlockBaseAddress(frame, []) }
        memset(CVPixelBufferGetBaseAddressOfPlane(frame, 0), Int32(luma),
               CVPixelBufferGetBytesPerRowOfPlane(frame, 0) * CVPixelBufferGetHeightOfPlane(frame, 0))
        memset(CVPixelBufferGetBaseAddressOfPlane(frame, 1), 128,
               CVPixelBufferGetBytesPerRowOfPlane(frame, 1) * CVPixelBufferGetHeightOfPlane(frame, 1))
    }

    // Top half `top`, bottom half `bottom`, so a rotation shows where the top went.
    private func makeSplitFrame(width: Int, height: Int, top: UInt8, bottom: UInt8) throws -> CVPixelBuffer {
        let frame = try makeFrame(width: width, height: height, luma: bottom)
        CVPixelBufferLockBaseAddress(frame, [])
        defer { CVPixelBufferUnlockBaseAddress(frame, []) }
        memset(CVPixelBufferGetBaseAddressOfPlane(frame, 0), Int32(top),
               CVPixelBufferGetBytesPerRowOfPlane(frame, 0) * height / 2)
        return frame
    }

    private func luma(_ frame: CVPixelBuffer, x: Int, y: Int) -> UInt8 {
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        let base = CVPixelBufferGetBaseAddressOfPlane(frame, 0)!.assumingMemoryBound(to: UInt8.self)
        return base[y * CVPixelBufferGetBytesPerRowOfPlane(frame, 0) + x]
    }

    @Test func landscapeFramePassesThrough() throws {
        let fitter = try #require(FrameFitter(width: 1920, height: 1080))
        let frame = try makeFrame(width: 1920, height: 1080, luma: 200)
        #expect(fitter.fit(frame) === frame)
    }

    // A phone held upright streams a tall picture with black bars, at the same size.
    @Test func portraitFrameIsPillarboxed() throws {
        let fitter = try #require(FrameFitter(width: 1920, height: 1080))
        let frame = try makeFrame(width: 1080, height: 1920, luma: 235)

        // Dirty a pooled buffer first: recycled buffers must still get black bars.
        if let first = fitter.fit(frame) { fill(first, luma: 235) }

        let out = try #require(fitter.fit(frame))
        #expect(CVPixelBufferGetWidth(out) == 1920)
        #expect(CVPixelBufferGetHeight(out) == 1080)
        #expect(luma(out, x: 10, y: 540) <= 20)
        #expect(luma(out, x: 1909, y: 540) <= 20)
        #expect(luma(out, x: 960, y: 540) >= 200)
    }
}

extension FrameFitterTests {
    // The camera always delivers landscape; a phone held upright needs a quarter
    // turn, then the tall result is pillarboxed back into the same 16:9 size.
    @Test func quarterTurnIsRotatedThenPillarboxed() throws {
        let fitter = try #require(FrameFitter(width: 1920, height: 1080))
        let frame = try makeSplitFrame(width: 1920, height: 1080, top: 235, bottom: 100)
        let out = try #require(fitter.fit(frame, rotation: 90))

        #expect(CVPixelBufferGetWidth(out) == 1920)
        #expect(CVPixelBufferGetHeight(out) == 1080)
        // Clockwise: the old top is now the right half of the centred picture.
        #expect(luma(out, x: 10, y: 540) <= 20)
        #expect(luma(out, x: 1100, y: 540) >= 200)
        #expect((80...120).contains(luma(out, x: 820, y: 540)))
    }

    @Test func halfTurnKeepsTheSizeAndFlips() throws {
        let fitter = try #require(FrameFitter(width: 1920, height: 1080))
        let frame = try makeSplitFrame(width: 1920, height: 1080, top: 235, bottom: 100)
        let out = try #require(fitter.fit(frame, rotation: 180))

        #expect(CVPixelBufferGetWidth(out) == 1920)
        #expect((80...120).contains(luma(out, x: 960, y: 100)))
        #expect(luma(out, x: 960, y: 980) >= 200)
    }
}
