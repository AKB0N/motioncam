package com.motioncam.camera;

import android.graphics.Bitmap;
import android.graphics.PointF;
import android.util.Size;
import android.view.Surface;

import com.motioncam.processor.NativeBitmapListener;
import com.squareup.moshi.JsonAdapter;
import com.squareup.moshi.Moshi;

import java.io.IOException;

public class NativeCameraSessionBridge implements NativeCameraSessionListener, NativeCameraRawPreviewListener {
    public final static long INVALID_NATIVE_HANDLE = -1;

    // Camera states
    public enum CameraState {
        INVALID(-1),
        READY(0),
        ACTIVE(1),
        CLOSED(2);

        private final int mState;

        CameraState(int state) {
            mState = state;
        }

        static public CameraState FromInt(int state) {
            for(CameraState cameraState : CameraState.values()) {
                if(cameraState.mState == state)
                    return cameraState;
            }

            return INVALID;
        }
    }

    public enum CameraFocusState {
        INVALID(-1),
        INACTIVE(0),
        PASSIVE_SCAN(1),
        PASSIVE_FOCUSED(2),
        ACTIVE_SCAN(3),
        FOCUS_LOCKED(4),
        NOT_FOCUS_LOCKED(5),
        PASSIVE_UNFOCUSED(6);

        private final int mState;

        CameraFocusState(int state) {
            mState = state;
        }

        static public CameraFocusState FromInt(int state) {
            for(CameraFocusState cameraFocusState : CameraFocusState.values()) {
                if(cameraFocusState.mState == state)
                    return cameraFocusState;
            }

            return INVALID;
        }
    }

    public enum CameraExposureState {
        INVALID(-1),
        INACTIVE(0),
        SEARCHING(1),
        CONVERGED(2),
        LOCKED(3),
        FLASH_REQUIRED(4),
        PRECAPTURE(5);

        private final int mState;

        CameraExposureState(int state) {
            mState = state;
        }

        static public CameraExposureState FromInt(int state) {
            for(CameraExposureState cameraExposureState : CameraExposureState.values()) {
                if(cameraExposureState.mState == state)
                    return cameraExposureState;
            }

            return INVALID;
        }
    }

    public interface CameraSessionListener {
        void onCameraStarted();
        void onCameraDisconnected();
        void onCameraError(int error);
        void onCameraSessionStateChanged(CameraState state);
        void onCameraExposureStatus(int iso, long exposureTime);
        void onCameraAutoFocusStateChanged(CameraFocusState state, float focusDistance);
        void onCameraAutoExposureStateChanged(CameraExposureState state);
        void onCameraHdrImageCaptureProgress(int progress);
        void onCameraHdrImageCaptureFailed();
        void onCameraHdrImageCaptureCompleted();

        void onMemoryAdjusting();
        void onMemoryStable();
    }

    public interface CameraRawPreviewListener {
        void onRawPreviewCreated(Bitmap bitmap);
        void onRawPreviewUpdated();
    }

    public static class CameraException extends RuntimeException {
        CameraException(String error) {
            super(error);
        }
    }

    private final Moshi mJson = new Moshi.Builder().build();
    private long mNativeCameraHandle;
    private CameraSessionListener mListener;
    private CameraRawPreviewListener mRawPreviewListener;

    public NativeCameraSessionBridge(long nativeHandle) {
        mNativeCameraHandle = nativeHandle;

        // Add to reference count
        Acquire(nativeHandle);
    }

    public NativeCameraSessionBridge(CameraSessionListener cameraListener, long maxMemoryUsageBytes, String nativeLibPath) {
        mNativeCameraHandle = Create(this, maxMemoryUsageBytes, nativeLibPath);
        mListener = cameraListener;

        if(mNativeCameraHandle == INVALID_NATIVE_HANDLE) {
            throw new CameraException(GetLastError());
        }
    }

    private void ensureValidHandle() {
        if(mNativeCameraHandle == INVALID_NATIVE_HANDLE) {
            throw new CameraException("Camera not ready");
        }
    }

    public long getHandle() {
        return mNativeCameraHandle;
    }

    public void destroy() {
        ensureValidHandle();

        DestroyImageProcessor();
        Destroy(mNativeCameraHandle);

        mListener = null;
        mNativeCameraHandle = INVALID_NATIVE_HANDLE;
    }

    public NativeCameraInfo[] getSupportedCameras() {
        ensureValidHandle();

        return GetSupportedCameras(mNativeCameraHandle);
    }

    public void startCapture(
            NativeCameraInfo cameraInfo,
            Surface previewOutput,
            boolean setupForRawPreview,
            boolean preferRaw12,
            boolean preferRaw16,
            CameraStartupSettings startupSettings)
    {
        ensureValidHandle();

        JsonAdapter<CameraStartupSettings> jsonAdapter = mJson.adapter(CameraStartupSettings.class);
        String startupSettingsJson = jsonAdapter.toJson(startupSettings);

        if(!StartCapture(
                mNativeCameraHandle,
                cameraInfo.cameraId,
                previewOutput,
                setupForRawPreview,
                preferRaw12,
                preferRaw16,
                startupSettingsJson))
        {
            throw new CameraException(GetLastError());
        }
    }

    public void stopCapture() {
        ensureValidHandle();

        if(!StopCapture(mNativeCameraHandle)) {
            throw new CameraException(GetLastError());
        }
    }

    public void pauseCapture() {
        ensureValidHandle();

        PauseCapture(mNativeCameraHandle);
    }

    public void resumeCapture() {
        ensureValidHandle();

        ResumeCapture(mNativeCameraHandle);
    }

    public void setAutoExposure() {
        ensureValidHandle();

        SetAutoExposure(mNativeCameraHandle);
    }

    public void setAELock(boolean lock) {
        ensureValidHandle();

        SetAELock(mNativeCameraHandle, lock);
    }

    public void setOIS(boolean ois) {
        ensureValidHandle();

        SetOIS(mNativeCameraHandle, ois);
    }

    public void setManualFocus(float focusDistance) {
        ensureValidHandle();

        SetManualFocus(mNativeCameraHandle, focusDistance);
    }

    public void setFocusForVideo(boolean focusForVideo) {
        ensureValidHandle();

        SetFocusForVideo(mNativeCameraHandle, focusForVideo);
    }

    public void setAWBLock(boolean lock) {
        ensureValidHandle();

        SetAWBLock(mNativeCameraHandle, lock);
    }

    public void setManualExposureValues(int iso, long exposureValue) {
        ensureValidHandle();

        SetManualExposure(mNativeCameraHandle, iso, exposureValue);
    }

    public void setExposureCompensation(float value) {
        ensureValidHandle();

        SetExposureCompensation(mNativeCameraHandle, value);
    }

    public Size getRawConfigurationOutput(NativeCameraInfo cameraInfo) {
        ensureValidHandle();

        Size rawOutput = GetRawOutputSize(mNativeCameraHandle, cameraInfo.cameraId);
        if(rawOutput == null) {
            throw new CameraException(GetLastError());
        }

        return rawOutput;
    }

    public PostProcessSettings getRawPreviewEstimatedPostProcessSettings() throws IOException {
        ensureValidHandle();

        String settingsJson = GetRawPreviewEstimatedSettings(mNativeCameraHandle);

        if(settingsJson == null) {
            return null;
        }

        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        return jsonAdapter.fromJson(settingsJson);
    }

    public Size getPreviewConfigurationOutput(NativeCameraInfo cameraInfo, Size captureSize, Size displaySize) {
        ensureValidHandle();

        Size previewOutput = GetPreviewOutputSize(mNativeCameraHandle, cameraInfo.cameraId, captureSize, displaySize);
        if(previewOutput == null) {
            throw new CameraException(GetLastError());
        }

        return previewOutput;
    }

    public void prepareHdrCapture(int iso, long exposure) {
        ensureValidHandle();

        PrepareHdrCapture(mNativeCameraHandle, iso, exposure);
    }

    public void captureImage(long bufferHandle, int numSaveImages, PostProcessSettings settings, String outputPath) {
        ensureValidHandle();

        // Serialize settings to json and pass to native code
        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        String json = jsonAdapter.toJson(settings);

        CaptureImage(mNativeCameraHandle, bufferHandle, numSaveImages, json, outputPath);
    }

    public void captureZslHdrImage(int numSaveImages, PostProcessSettings settings, String outputPath) {
        ensureValidHandle();

        // Serialize settings to json and pass to native code
        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        String json = jsonAdapter.toJson(settings);

        CaptureZslHdrImage(mNativeCameraHandle, numSaveImages, json, outputPath);
    }

    public void captureHdrImage(int numSaveImages, int baseIso, long baseExposure, int hdrIso, long hdrExposure, PostProcessSettings settings, String outputPath) {
        ensureValidHandle();

        // Serialize settings to json and pass to native code
        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        String json = jsonAdapter.toJson(settings);

        CaptureHdrImage(mNativeCameraHandle, numSaveImages, baseIso, baseExposure, hdrIso, hdrExposure, json, outputPath);
    }

    public Size getPreviewSize(int downscaleFactor) {
        ensureValidHandle();

        return GetPreviewSize(mNativeCameraHandle, downscaleFactor);
    }

    public void createPreviewImage(long bufferHandle, PostProcessSettings settings, int downscaleFactor, Bitmap dst) {
        if(mNativeCameraHandle == INVALID_NATIVE_HANDLE) {
            throw new CameraException("Camera not ready");
        }

        // Serialize settings to json and pass to native code
        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        String json = jsonAdapter.toJson(settings);

        CreateImagePreview(mNativeCameraHandle, bufferHandle, json, downscaleFactor, dst);
    }

    public NativeCameraBuffer[] getAvailableImages() {
        ensureValidHandle();

        return GetAvailableImages(mNativeCameraHandle);
    }

    public PostProcessSettings estimatePostProcessSettings(float shadowsBias) throws IOException {
        ensureValidHandle();

        String settingsJson = EstimatePostProcessSettings(mNativeCameraHandle, shadowsBias);
        if(settingsJson == null) {
            return null;
        }

        JsonAdapter<PostProcessSettings> jsonAdapter = mJson.adapter(PostProcessSettings.class);
        return jsonAdapter.fromJson(settingsJson);
    }

    public float estimateShadows(float bias) {
        ensureValidHandle();

        return EstimateShadows(mNativeCameraHandle, bias);
    }

    public double measureSharpness(long bufferHandle) {
        ensureValidHandle();

        return MeasureSharpness(mNativeCameraHandle, bufferHandle);
    }

    public NativeCameraMetadata getMetadata(NativeCameraInfo cameraInfo) {
        ensureValidHandle();

        return GetMetadata(mNativeCameraHandle, cameraInfo.cameraId);
    }

    public void enableRawPreview(CameraRawPreviewListener listener, int previewQuality, boolean overrideWb) {
        ensureValidHandle();

        mRawPreviewListener = listener;

        EnableRawPreview(mNativeCameraHandle, this, previewQuality, overrideWb);
    }

    public void setRawPreviewSettings(
            float shadows,
            float contrast,
            float saturation,
            float blacks,
            float whitePoint,
            float tempOffset,
            float tintOffset,
            boolean useVideoPreview)
    {
        ensureValidHandle();

        SetRawPreviewSettings(mNativeCameraHandle, shadows, contrast, saturation, blacks, whitePoint, tempOffset, tintOffset, useVideoPreview);
    }

    public void disableRawPreview() {
        ensureValidHandle();

        DisableRawPreview(mNativeCameraHandle);
    }

    public void updateOrientation(NativeCameraBuffer.ScreenOrientation orientation) {
        ensureValidHandle();

        UpdateOrientation(mNativeCameraHandle, orientation.value);
    }

    public void setFocusPoint(PointF focusPt, PointF exposurePt) {
        ensureValidHandle();

        SetFocusPoint(mNativeCameraHandle, focusPt.x, focusPt.y, exposurePt.x, exposurePt.y);
    }

    public void setAutoFocus() {
        ensureValidHandle();

        SetAutoFocus(mNativeCameraHandle);
    }

    public void streamToFile(int[] fds, int audioFd, int audioDeviceId, boolean enableCompression, int numThreads) {
        ensureValidHandle();

        StartStreamToFile(mNativeCameraHandle, fds, audioFd, audioDeviceId, enableCompression, numThreads);
    }

    public void adjustMemory(long maxMemoryBytes) {
        ensureValidHandle();

        AdjustMemoryUse(mNativeCameraHandle, maxMemoryBytes);
    }

    public void endStream() {
        ensureValidHandle();

        EndStream(mNativeCameraHandle);
    }

    public void setFrameRate(int frameRate) {
        ensureValidHandle();

        SetFrameRate(mNativeCameraHandle, frameRate);
    }

    public void setVideoBin(boolean bin) {
        ensureValidHandle();

        SetVideoBin(mNativeCameraHandle, bin);
    }

    public void setVideoCropPercentage(int horizontal, int vertical) {
        ensureValidHandle();

        SetVideoCropPercentage(mNativeCameraHandle, horizontal, vertical);
    }

    public VideoRecordingStats getVideoRecordingStats() {
        ensureValidHandle();

        return GetVideoRecordingStats(mNativeCameraHandle);
    }

    public Bitmap generateStats(NativeBitmapListener listener) {
        ensureValidHandle();

        return GenerateStats(mNativeCameraHandle, listener);
    }

    @Override
    public void onCameraStarted() {
        mListener.onCameraStarted();
    }

    @Override
    public void onCameraDisconnected() {
        mListener.onCameraDisconnected();
    }

    @Override
    public void onCameraError(int error) {
        mListener.onCameraError(error);
    }

    @Override
    public void onCameraSessionStateChanged(int state) {
        mListener.onCameraSessionStateChanged(CameraState.FromInt(state));
    }

    @Override
    public void onCameraExposureStatus(int iso, long exposureTime) {
        mListener.onCameraExposureStatus(iso, exposureTime);
    }

    @Override
    public void onCameraAutoFocusStateChanged(int state, float focusDistance) {
        mListener.onCameraAutoFocusStateChanged(CameraFocusState.FromInt(state), focusDistance);
    }

    @Override
    public void onCameraAutoExposureStateChanged(int state) {
        mListener.onCameraAutoExposureStateChanged(CameraExposureState.FromInt(state));
    }

    @Override
    public void onCameraHdrImageCaptureFailed() {
        mListener.onCameraHdrImageCaptureFailed();
    }

    @Override
    public void onCameraHdrImageCaptureProgress(int image) {
        mListener.onCameraHdrImageCaptureProgress(image);
    }

    @Override
    public void onCameraHdrImageCaptureCompleted() {
        mListener.onCameraHdrImageCaptureCompleted();
    }

    @Override
    public void onMemoryAdjusting() {
        mListener.onMemoryAdjusting();
    }

    @Override
    public void onMemoryStable() {
        mListener.onMemoryStable();
    }

    @Override
    public Bitmap onRawPreviewBitmapNeeded(int width, int height) {
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);

        mRawPreviewListener.onRawPreviewCreated(bitmap);

        return bitmap;
    }

    public void initImageProcessor()
    {
        InitImageProcessor();
    }

    @Override
    public void onRawPreviewUpdated() {
        mRawPreviewListener.onRawPreviewUpdated();
    }

    private native long Create(NativeCameraSessionListener listener, long maxMemoryUsageBytes, String nativeLibPath);
    private native void Destroy(long handle);
    private native boolean Acquire(long handle);

    private native String GetLastError();

    private native NativeCameraInfo[] GetSupportedCameras(long handle);

    private native boolean StartCapture(
            long handle,
            String cameraId,
            Surface previewSurface,
            boolean setupForRawPreview,
            boolean preferRaw12,
            boolean preferRaw16,
            String cameraStartupSettingsJson);

    private native boolean StopCapture(long handle);

    private native boolean PauseCapture(long handle);
    private native boolean ResumeCapture(long handle);

    private native boolean SetManualExposure(long handle, int iso, long exposureTime);
    private native boolean SetAutoExposure(long handle);
    private native boolean SetExposureCompensation(long handle, float value);
    private native boolean SetAWBLock(long handle, boolean lock);
    private native boolean SetAELock(long handle, boolean lock);
    private native boolean SetOIS(long handle, boolean on);
    private native boolean SetManualFocus(long handle, float focusDistance);
    private native boolean SetFocusForVideo(long handle, boolean focusForVideo);

    private native boolean EnableRawPreview(long handle, NativeCameraRawPreviewListener listener, int previewQuality, boolean overrideWb);
    private native boolean SetRawPreviewSettings(long handle, float shadows, float contrast, float saturation, float blacks, float whitePoint, float tempOffset, float tintOfset, boolean useVideoPreview);
    private native boolean DisableRawPreview(long handle);
    private native String GetRawPreviewEstimatedSettings(long handle);

    private native boolean SetFocusPoint(long handle, float focusX, float focusY, float exposureX, float exposureY);
    private native boolean SetAutoFocus(long handle);

    private native boolean UpdateOrientation(long handle, int orientation);

    private native void InitImageProcessor();
    private native void DestroyImageProcessor();
    private native NativeCameraMetadata GetMetadata(long handle, String cameraId);
    private native Size GetRawOutputSize(long handle, String cameraId);
    private native Size GetPreviewOutputSize(long handle, String cameraId, Size captureSize, Size displaySize);

    private native boolean StartStreamToFile(long handle, int[] videoFds, int audioFd, int audioDeviceId, boolean enableCompression, int numThreads);
    private native void EndStream(long handle);

    private native void PrepareHdrCapture(long handle, int iso, long exposure);
    private native boolean CaptureImage(long handle, long bufferHandle, int numSaveImages, String settings, String outputPath);

    private native boolean CaptureZslHdrImage(long handle, int numImages, String settings, String outputPath);
    private native boolean CaptureHdrImage(long handle, int numImages, int baseIso, long baseExposure, int hdrIso, long hdrExposure, String settings, String outputPath);

    private native NativeCameraBuffer[] GetAvailableImages(long handle);
    private native Size GetPreviewSize(long handle, int downscaleFactor);
    private native boolean CreateImagePreview(long handle, long bufferHandle, String settings, int downscaleFactor, Bitmap dst);

    private native double MeasureSharpness(long handle, long bufferHandle);

    private native float EstimateShadows(long handle, float bias);
    private native String EstimatePostProcessSettings(long bufferHandle, float shadowsBias);

    private native void SetFrameRate(long handle, int frameRate);
    private native void SetVideoCropPercentage(long handle, int horizontal, int vertical);
    private native VideoRecordingStats GetVideoRecordingStats(long handle);
    private native void SetVideoBin(long handle, boolean bin);

    private native void AdjustMemoryUse(long handle, long maxUseBytes);

    private native Bitmap GenerateStats(long handle, NativeBitmapListener listener);
}
