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
        VStack(spacing: 16) {
            CameraPreview(session: viewModel.camera.session)
                .aspectRatio(16 / 9, contentMode: .fit)
                .background(Color.black)
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .accessibilityLabel("Camera preview")

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
        .padding()
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

    func updateUIView(_ uiView: PreviewView, context: Context) {}
}
