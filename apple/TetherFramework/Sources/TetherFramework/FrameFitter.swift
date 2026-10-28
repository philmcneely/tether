//
//  FrameFitter.swift
//  TetherFramework
//
//  Turns camera frames upright and fits them into one fixed size, so the
//  desktop's webcam never changes resolution when the phone rotates.
//

import CoreVideo
import VideoToolbox

final class FrameFitter {
    let width: Int
    let height: Int

    private let pixelFormat: OSType
    private let rotator: VTPixelRotationSession
    private let transfer: VTPixelTransferSession
    // Keyed by "WxH": the output size, plus the tall size for quarter turns.
    private var pools: [String: CVPixelBufferPool] = [:]
    private var quarterTurns = 0

    init?(width: Int, height: Int, pixelFormat: OSType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        var rotator: VTPixelRotationSession?
        var transfer: VTPixelTransferSession?
        guard VTPixelRotationSessionCreate(nil, &rotator) == noErr,
              VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &transfer) == noErr,
              let rotator, let transfer
        else { return nil }
        // Scale to fit, filling the rest with black: pillarbox for portrait frames.
        VTSessionSetProperty(transfer, key: kVTPixelTransferPropertyKey_ScalingMode, value: kVTScalingMode_Letterbox)

        self.width = width
        self.height = height
        self.pixelFormat = pixelFormat
        self.rotator = rotator
        self.transfer = transfer
    }

    deinit {
        VTPixelRotationSessionInvalidate(rotator)
        VTPixelTransferSessionInvalidate(transfer)
    }

    // Turns the image clockwise by `angle` degrees (a multiple of 90), then pillarboxes
    // it to the output size. Returns the image itself when it needs neither.
    func fit(_ image: CVPixelBuffer, rotation angle: Double = 0) -> CVPixelBuffer? {
        let turns = ((Int((angle / 90).rounded()) % 4) + 4) % 4
        var source = image

        if turns != 0 {
            if turns != quarterTurns {
                let values = [kVTRotation_0, kVTRotation_CW90, kVTRotation_180, kVTRotation_CCW90]
                VTSessionSetProperty(rotator, key: kVTPixelRotationPropertyKey_Rotation, value: values[turns])
                quarterTurns = turns
            }
            let w = CVPixelBufferGetWidth(image), h = CVPixelBufferGetHeight(image)
            guard let turned = makeBuffer(width: turns.isMultiple(of: 2) ? w : h,
                                          height: turns.isMultiple(of: 2) ? h : w),
                  VTPixelRotationSessionRotateImage(rotator, image, turned) == noErr
            else { return nil }
            source = turned
        }

        if CVPixelBufferGetWidth(source) == width && CVPixelBufferGetHeight(source) == height {
            return source
        }
        guard let out = makeBuffer(width: width, height: height),
              VTPixelTransferSessionTransferImage(transfer, from: source, to: out) == noErr
        else { return nil }
        return out
    }

    private func makeBuffer(width: Int, height: Int) -> CVPixelBuffer? {
        let key = "\(width)x\(height)"
        if pools[key] == nil {
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: pixelFormat,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
            ]
            var pool: CVPixelBufferPool?
            guard CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &pool) == kCVReturnSuccess
            else { return nil }
            pools[key] = pool
        }
        var buffer: CVPixelBuffer?
        guard let pool = pools[key],
              CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer) == kCVReturnSuccess
        else { return nil }
        return buffer
    }
}
