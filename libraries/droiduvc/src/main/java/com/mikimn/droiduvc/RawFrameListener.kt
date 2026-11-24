package com.mikimn.droiduvc

import java.nio.ByteBuffer

/**
 * Optional observer for the original UVC payload. The direct buffer is borrowed from libuvc
 * and is valid only during this callback. Implementations must copy immediately, return quickly,
 * and must not call UvcCamera methods from the streaming callback thread.
 *
 * formatCode is the native libuvc uvc_frame_format enum value.
 * captureTimeNs is the native capture-finished timestamp when supplied by libuvc, otherwise 0.
 */
fun interface RawFrameListener {
    fun onFrame(
        data: ByteBuffer,
        width: Int,
        height: Int,
        formatCode: Int,
        sequence: Long,
        captureTimeNs: Long
    )
}
