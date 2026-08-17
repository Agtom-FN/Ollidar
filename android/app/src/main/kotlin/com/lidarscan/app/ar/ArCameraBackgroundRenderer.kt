package com.lidarscan.app.ar

import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import com.google.ar.core.Coordinates2d
import com.google.ar.core.Frame
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Draws the ARCore camera image as a full-screen quad from an external OES
 * texture, and drives [CaptureArController.onFrame] from the GL thread.
 *
 * ### Why a second surface instead of Filament's external-texture path
 *
 * B7 could put the camera image into the Filament scene directly, the way
 * Filament's own ArCore sample does: a `Texture` with
 * `Sampler.SAMPLER_EXTERNAL` fed by a `Stream` built from an OES texture id,
 * sampled by a `samplerExternal` material. That is one surface, one context,
 * and correct depth interaction. It was **not** chosen here, for two concrete
 * reasons rather than a preference:
 *
 *  1. **`samplerExternal` is an OpenGL-only material feature.** This app's
 *     `compileMaterials` task compiles every `.mat` with `-a opengl -a vulkan`
 *     (B4), and `matc` cannot emit a Vulkan variant of a `samplerExternal`
 *     material. Taking that path means either splitting the material pipeline
 *     per-backend or pinning the whole app to `Engine.Backend.OPENGL` — a
 *     change to B4's renderer that could not be validated here.
 *  2. **The external texture must live in a context shared with Filament's
 *     driver context**, and Filament creates that context internally on its
 *     own thread. Getting the sharing right is exactly the kind of thing that
 *     works or does not work on a specific device, and **no ARCore device was
 *     available to this task** — shipping an unverifiable single-surface path
 *     would be worse than shipping a verifiable two-surface one.
 *
 * So AR mode composes two surfaces: this `GLSurfaceView` underneath drawing
 * the camera, and B4's Filament `SurfaceView` on top in translucent mode
 * (`UiHelper.setOpaque(false)` + `setMediaOverlay(true)` + `View.BlendMode
 * .TRANSLUCENT` + a zero-alpha clear). The costs are honest and small: two GL
 * contexts, and one extra full-screen composite by SurfaceFlinger. The
 * single-surface path is the right follow-up **once a device exists to
 * validate it on**; nothing else in the app has to change to take it.
 *
 * ### Texture coordinates
 *
 * The quad's UVs are not hardcoded: `Frame.transformCoordinates2d` converts
 * the fixed NDC quad into the texture coordinates that account for the
 * device's rotation, the camera's aspect and any cropping, and it is
 * re-run whenever `Frame.hasDisplayGeometryChanged()`. Hardcoding them is the
 * standard way to end up with a mirrored or stretched preview on one device
 * and not another.
 */
class ArCameraBackgroundRenderer(
    private val controller: CaptureArController,
    /**
     * ROUND 5 AUDIT bugfix: which of the (at most one, by design) renderers
     * that can be alive at once this instance is — [ArPosePumpView]'s pump or
     * [ArOverlayView]'s overlay. Every call into [controller] carries it, so a
     * renderer whose `AndroidView` has already been superseded (see
     * `CaptureArController.RendererOwner`'s doc) safely becomes a no-op on the
     * session instead of racing the new one for it.
     */
    private val owner: CaptureArController.RendererOwner,
    /** Invoked on the GL thread after each ARCore frame, for consumers that need it (the overlay camera). */
    private val onFrame: (Frame) -> Unit = {},
) : GLSurfaceView.Renderer {

    private var program = 0
    private var positionAttribute = 0
    private var texCoordAttribute = 0
    private var textureUniform = 0
    private var textureId = -1

    private val quadCoords: FloatBuffer = floatBuffer(
        floatArrayOf(-1f, -1f, +1f, -1f, -1f, +1f, +1f, +1f),
    )
    private val quadTexCoords: FloatBuffer = floatBuffer(FloatArray(8))

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)

        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        controller.setCameraTextureName(textureId, owner)

        program = buildProgram()
        positionAttribute = GLES20.glGetAttribLocation(program, "a_Position")
        texCoordAttribute = GLES20.glGetAttribLocation(program, "a_TexCoord")
        textureUniform = GLES20.glGetUniformLocation(program, "u_Texture")
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        controller.setDisplayGeometry(null, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        if (textureId >= 0) controller.setCameraTextureName(textureId, owner)

        val frame = controller.onFrame(owner) ?: return
        if (frame.hasDisplayGeometryChanged()) updateTexCoords(frame)
        drawBackground()
        onFrame(frame)
    }

    private fun updateTexCoords(frame: Frame) {
        val src = FloatArray(8)
        quadCoords.rewind()
        quadCoords.get(src)
        quadCoords.rewind()
        val dst = FloatArray(8)
        frame.transformCoordinates2d(
            Coordinates2d.OPENGL_NORMALIZED_DEVICE_COORDINATES, src,
            Coordinates2d.TEXTURE_NORMALIZED, dst,
        )
        quadTexCoords.rewind()
        quadTexCoords.put(dst)
        quadTexCoords.rewind()
    }

    private fun drawBackground() {
        // The camera image is the backdrop: no depth test, no depth write, so
        // it never occludes the point cloud drawn over it by the Filament
        // surface above.
        GLES20.glDisable(GLES20.GL_DEPTH_TEST)
        GLES20.glDepthMask(false)

        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureUniform, 0)

        GLES20.glVertexAttribPointer(positionAttribute, 2, GLES20.GL_FLOAT, false, 0, quadCoords)
        GLES20.glVertexAttribPointer(texCoordAttribute, 2, GLES20.GL_FLOAT, false, 0, quadTexCoords)
        GLES20.glEnableVertexAttribArray(positionAttribute)
        GLES20.glEnableVertexAttribArray(texCoordAttribute)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(positionAttribute)
        GLES20.glDisableVertexAttribArray(texCoordAttribute)

        GLES20.glDepthMask(true)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
    }

    private fun buildProgram(): Int {
        val vertex = compile(
            GLES20.GL_VERTEX_SHADER,
            """
            attribute vec4 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() {
                gl_Position = a_Position;
                v_TexCoord = a_TexCoord;
            }
            """.trimIndent(),
        )
        val fragment = compile(
            GLES20.GL_FRAGMENT_SHADER,
            """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 v_TexCoord;
            uniform samplerExternalOES u_Texture;
            void main() {
                gl_FragColor = texture2D(u_Texture, v_TexCoord);
            }
            """.trimIndent(),
        )
        val id = GLES20.glCreateProgram()
        GLES20.glAttachShader(id, vertex)
        GLES20.glAttachShader(id, fragment)
        GLES20.glLinkProgram(id)
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)
        return id
    }

    private fun compile(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        check(status[0] != 0) { "camera background shader failed: ${GLES20.glGetShaderInfoLog(shader)}" }
        return shader
    }

    private fun floatBuffer(values: FloatArray): FloatBuffer =
        ByteBuffer.allocateDirect(values.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
            put(values)
            rewind()
        }
}
