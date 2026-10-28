//
//  CameraView.swift
//  Tether
//
//  Streams the camera to the desktop, where it shows up as a webcam.
//

import AVFoundation
import SwiftUI
import TetherFramework

struct CameraView: View {
    @Environment(TetherViewModel.self) private var viewModel
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @State private var position: AVCaptureDevice.Position = .back

    var body: some View {
        NavigationStack {
            Group {
                if viewModel.appState != .connected {
                    message("Connect to a device to use the camera")
                } else if viewModel.daemonCameraPort == nil {
                    message("This desktop's Tether does not support the camera")
                } else {
                    streamView
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Camera")
        }
        .onChange(of: viewModel.camera.isStreaming) { _, streaming in
            // The phone must stay awake and in front to keep streaming.
            UIApplication.shared.isIdleTimerDisabled = streaming
        }
    }

    private var streamView: some View {
        // AnyLayout keeps the preview's identity
        let landscape = verticalSizeClass == .compact
        let layout = landscape
            ? AnyLayout(HStackLayout(alignment: .top, spacing: 16))
            : AnyLayout(VStackLayout(spacing: 16))
        return layout {
            preview
            VStack(spacing: 16) { controls }
                .frame(maxWidth: landscape ? 280 : .infinity)
        }
        .padding()
    }

    // 16:9 like the stream; a portrait phone shows pillarboxed, as the desktop sees it.
    private var preview: some View {
        CameraPreview(session: viewModel.camera.session, device: viewModel.camera.device)
            .aspectRatio(16 / 9, contentMode: .fit)
            .background(Color.black)
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .accessibilityLabel("Camera preview")
    }

    @ViewBuilder
    private var controls: some View {
        Picker("Camera", selection: $position) {
            Text("Back").tag(AVCaptureDevice.Position.back)
            Text("Front").tag(AVCaptureDevice.Position.front)
        }
        .pickerStyle(.segmented)
        .disabled(viewModel.camera.isStreaming)

        if viewModel.camera.isStreaming {
            Button("Stop", role: .destructive) { viewModel.camera.stop() }
                .buttonStyle(.borderedProminent)
        } else {
            Button("Start Streaming") { viewModel.startCamera(position: position) }
                .buttonStyle(.borderedProminent)
        }

        if let error = viewModel.camera.lastError {
            Text(error)
                .font(.footnote)
                .foregroundStyle(.red)
        }

        Text("Keep Tether open while streaming. On the desktop, pick the camera named after this phone.")
            .font(.footnote)
            .foregroundStyle(.secondary)
            .multilineTextAlignment(.center)

        Spacer()
    }

    private func message(_ text: LocalizedStringKey) -> some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "web.camera")
                .font(.system(size: 48))
                .foregroundStyle(.tertiary)
                .accessibilityHidden(true)
            Text(text)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }
}

private struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    let device: AVCaptureDevice?

    // Keeps the preview upright as the phone and the UI rotate.
    final class Coordinator {
        var device: AVCaptureDevice?
        var rotation: AVCaptureDevice.RotationCoordinator?
        var observation: NSKeyValueObservation?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspect
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        let coordinator = context.coordinator
        guard coordinator.device !== device else { return }
        coordinator.device = device
        coordinator.observation = nil
        coordinator.rotation = nil
        guard let device else { return }

        let layer = uiView.previewLayer
        let rotation = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: layer)
        coordinator.observation = rotation.observe(\.videoRotationAngleForHorizonLevelPreview,
                                                   options: [.initial, .new]) { [weak layer] rotation, _ in
            let angle = rotation.videoRotationAngleForHorizonLevelPreview
            DispatchQueue.main.async {
                if let connection = layer?.connection, connection.isVideoRotationAngleSupported(angle) {
                    connection.videoRotationAngle = angle
                }
            }
        }
        coordinator.rotation = rotation
    }
}
