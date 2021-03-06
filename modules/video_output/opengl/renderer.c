/*****************************************************************************
 * renderer.c
 *****************************************************************************
 * Copyright (C) 2004-2020 VLC authors and VideoLAN
 * Copyright (C) 2009, 2011 Laurent Aimar
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *          Ilkka Ollakka <ileoo@videolan.org>
 *          Rémi Denis-Courmont
 *          Adrien Maglo <magsoft at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "renderer.h"

#include <assert.h>
#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_picture.h>

#include "gl_util.h"
#include "internal.h"
#include "interop.h"
#include "vout_helper.h"

#define SPHERE_RADIUS 1.f

static const GLfloat identity[] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

static void getZoomMatrix(float zoom, GLfloat matrix[static 16]) {

    const GLfloat m[] = {
        /* x   y     z     w */
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, zoom, 1.0f
    };

    memcpy(matrix, m, sizeof(m));
}

/* perspective matrix see https://www.opengl.org/sdk/docs/man2/xhtml/gluPerspective.xml */
static void getProjectionMatrix(float sar, float fovy, GLfloat matrix[static 16]) {

    float zFar  = 1000;
    float zNear = 0.01;

    float f = 1.f / tanf(fovy / 2.f);

    const GLfloat m[] = {
        f / sar, 0.f,                   0.f,                0.f,
        0.f,     f,                     0.f,                0.f,
        0.f,     0.f,     (zNear + zFar) / (zNear - zFar), -1.f,
        0.f,     0.f, (2 * zNear * zFar) / (zNear - zFar),  0.f};

     memcpy(matrix, m, sizeof(m));
}

static void getViewpointMatrixes(struct vlc_gl_renderer *renderer,
                                 video_projection_mode_t projection_mode)
{
    if (projection_mode == PROJECTION_MODE_EQUIRECTANGULAR
        || projection_mode == PROJECTION_MODE_CUBEMAP_LAYOUT_STANDARD)
    {
        getProjectionMatrix(renderer->f_sar, renderer->f_fovy,
                            renderer->var.ProjectionMatrix);
        getZoomMatrix(renderer->f_z, renderer->var.ZoomMatrix);

        /* renderer->vp has been reversed and is a world transform */
        vlc_viewpoint_to_4x4(&renderer->vp, renderer->var.ViewMatrix);
    }
    else
    {
        memcpy(renderer->var.ProjectionMatrix, identity, sizeof(identity));
        memcpy(renderer->var.ZoomMatrix, identity, sizeof(identity));
        memcpy(renderer->var.ViewMatrix, identity, sizeof(identity));
    }

}

static void getOrientationTransformMatrix(video_orientation_t orientation,
                                          GLfloat matrix[static 16])
{
    memcpy(matrix, identity, sizeof(identity));

    const int k_cos_pi = -1;
    const int k_cos_pi_2 = 0;
    const int k_cos_n_pi_2 = 0;

    const int k_sin_pi = 0;
    const int k_sin_pi_2 = 1;
    const int k_sin_n_pi_2 = -1;

    switch (orientation) {

        case ORIENT_ROTATED_90:
            matrix[0 * 4 + 0] = k_cos_pi_2;
            matrix[0 * 4 + 1] = -k_sin_pi_2;
            matrix[1 * 4 + 0] = k_sin_pi_2;
            matrix[1 * 4 + 1] = k_cos_pi_2;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_ROTATED_180:
            matrix[0 * 4 + 0] = k_cos_pi;
            matrix[0 * 4 + 1] = -k_sin_pi;
            matrix[1 * 4 + 0] = k_sin_pi;
            matrix[1 * 4 + 1] = k_cos_pi;
            matrix[3 * 4 + 0] = 1;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_ROTATED_270:
            matrix[0 * 4 + 0] = k_cos_n_pi_2;
            matrix[0 * 4 + 1] = -k_sin_n_pi_2;
            matrix[1 * 4 + 0] = k_sin_n_pi_2;
            matrix[1 * 4 + 1] = k_cos_n_pi_2;
            matrix[3 * 4 + 0] = 1;
            break;
        case ORIENT_HFLIPPED:
            matrix[0 * 4 + 0] = -1;
            matrix[3 * 4 + 0] = 1;
            break;
        case ORIENT_VFLIPPED:
            matrix[1 * 4 + 1] = -1;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_TRANSPOSED:
            matrix[0 * 4 + 0] = 0;
            matrix[1 * 4 + 1] = 0;
            matrix[2 * 4 + 2] = -1;
            matrix[0 * 4 + 1] = 1;
            matrix[1 * 4 + 0] = 1;
            break;
        case ORIENT_ANTI_TRANSPOSED:
            matrix[0 * 4 + 0] = 0;
            matrix[1 * 4 + 1] = 0;
            matrix[2 * 4 + 2] = -1;
            matrix[0 * 4 + 1] = -1;
            matrix[1 * 4 + 0] = -1;
            matrix[3 * 4 + 0] = 1;
            matrix[3 * 4 + 1] = 1;
            break;
        default:
            break;
    }
}

static GLuint BuildVertexShader(const struct vlc_gl_renderer *renderer,
                                unsigned plane_count)
{
    const opengl_vtable_t *vt = renderer->vt;

    /* Basic vertex shader */
    static const char *template =
        "#version %u\n"
        "varying vec2 TexCoord0;\n"
        "attribute vec4 MultiTexCoord0;\n"
        "%s%s"
        "attribute vec3 VertexPosition;\n"
        "uniform mat4 TransformMatrix;\n"
        "uniform mat4 OrientationMatrix;\n"
        "uniform mat4 ProjectionMatrix;\n"
        "uniform mat4 ZoomMatrix;\n"
        "uniform mat4 ViewMatrix;\n"
        "void main() {\n"
        " TexCoord0 = vec4(TransformMatrix * OrientationMatrix * MultiTexCoord0).st;\n"
        "%s%s"
        " gl_Position = ProjectionMatrix * ZoomMatrix * ViewMatrix\n"
        "               * vec4(VertexPosition, 1.0);\n"
        "}";

    const char *coord1_header = plane_count > 1 ?
        "varying vec2 TexCoord1;\nattribute vec4 MultiTexCoord1;\n" : "";
    const char *coord1_code = plane_count > 1 ?
        " TexCoord1 = vec4(TransformMatrix * OrientationMatrix * MultiTexCoord1).st;\n" : "";
    const char *coord2_header = plane_count > 2 ?
        "varying vec2 TexCoord2;\nattribute vec4 MultiTexCoord2;\n" : "";
    const char *coord2_code = plane_count > 2 ?
        " TexCoord2 = vec4(TransformMatrix * OrientationMatrix * MultiTexCoord2).st;\n" : "";

    char *code;
    if (asprintf(&code, template, renderer->glsl_version, coord1_header,
                 coord2_header, coord1_code, coord2_code) < 0)
        return 0;

    GLuint shader = vt->CreateShader(GL_VERTEX_SHADER);
    vt->ShaderSource(shader, 1, (const char **) &code, NULL);
    if (renderer->b_dump_shaders)
        msg_Dbg(renderer->gl, "\n=== Vertex shader for fourcc: %4.4s ===\n%s\n",
                (const char *) &renderer->interop->fmt.i_chroma, code);
    vt->CompileShader(shader);
    free(code);
    return shader;
}

static int
opengl_link_program(struct vlc_gl_renderer *renderer)
{
    struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;

    GLuint vertex_shader = BuildVertexShader(renderer, interop->tex_count);
    if (!vertex_shader)
        return VLC_EGENERIC;

    GLuint fragment_shader =
        opengl_fragment_shader_init(renderer, interop->tex_target,
                                    interop->sw_fmt.i_chroma,
                                    interop->sw_fmt.space);
    if (!fragment_shader)
        return VLC_EGENERIC;

    assert(interop->tex_target != 0 &&
           interop->tex_count > 0 &&
           interop->ops->update_textures != NULL &&
           renderer->pf_fetch_locations != NULL &&
           renderer->pf_prepare_shader != NULL);

    GLuint shaders[] = { fragment_shader, vertex_shader };

    /* Check shaders messages */
    for (unsigned i = 0; i < 2; i++) {
        int infoLength;
        vt->GetShaderiv(shaders[i], GL_INFO_LOG_LENGTH, &infoLength);
        if (infoLength <= 1)
            continue;

        char *infolog = malloc(infoLength);
        if (infolog != NULL)
        {
            int charsWritten;
            vt->GetShaderInfoLog(shaders[i], infoLength, &charsWritten,
                                 infolog);
            msg_Err(renderer->gl, "shader %u: %s", i, infolog);
            free(infolog);
        }
    }

    GLuint program_id = renderer->program_id = vt->CreateProgram();
    vt->AttachShader(program_id, fragment_shader);
    vt->AttachShader(program_id, vertex_shader);
    vt->LinkProgram(program_id);

    vt->DeleteShader(vertex_shader);
    vt->DeleteShader(fragment_shader);

    /* Check program messages */
    int infoLength = 0;
    vt->GetProgramiv(program_id, GL_INFO_LOG_LENGTH, &infoLength);
    if (infoLength > 1)
    {
        char *infolog = malloc(infoLength);
        if (infolog != NULL)
        {
            int charsWritten;
            vt->GetProgramInfoLog(program_id, infoLength, &charsWritten,
                                  infolog);
            msg_Err(renderer->gl, "shader program: %s", infolog);
            free(infolog);
        }

        /* If there is some message, better to check linking is ok */
        GLint link_status = GL_TRUE;
        vt->GetProgramiv(program_id, GL_LINK_STATUS, &link_status);
        if (link_status == GL_FALSE)
        {
            msg_Err(renderer->gl, "Unable to use program");
            goto error;
        }
    }

    /* Fetch UniformLocations and AttribLocations */
#define GET_LOC(type, x, str) do { \
    x = vt->Get##type##Location(program_id, str); \
    assert(x != -1); \
    if (x == -1) { \
        msg_Err(renderer->gl, "Unable to Get"#type"Location(%s)", str); \
        goto error; \
    } \
} while (0)
#define GET_ULOC(x, str) GET_LOC(Uniform, renderer->uloc.x, str)
#define GET_ALOC(x, str) GET_LOC(Attrib, renderer->aloc.x, str)
    GET_ULOC(TransformMatrix, "TransformMatrix");
    GET_ULOC(OrientationMatrix, "OrientationMatrix");
    GET_ULOC(ProjectionMatrix, "ProjectionMatrix");
    GET_ULOC(ViewMatrix, "ViewMatrix");
    GET_ULOC(ZoomMatrix, "ZoomMatrix");

    GET_ALOC(VertexPosition, "VertexPosition");
    GET_ALOC(MultiTexCoord[0], "MultiTexCoord0");
    /* MultiTexCoord 1 and 2 can be optimized out if not used */
    if (interop->tex_count > 1)
        GET_ALOC(MultiTexCoord[1], "MultiTexCoord1");
    else
        renderer->aloc.MultiTexCoord[1] = -1;
    if (interop->tex_count > 2)
        GET_ALOC(MultiTexCoord[2], "MultiTexCoord2");
    else
        renderer->aloc.MultiTexCoord[2] = -1;
#undef GET_LOC
#undef GET_ULOC
#undef GET_ALOC
    int ret = renderer->pf_fetch_locations(renderer, program_id);
    assert(ret == VLC_SUCCESS);
    if (ret != VLC_SUCCESS)
    {
        msg_Err(renderer->gl, "Unable to get locations from tex_conv");
        goto error;
    }

    return VLC_SUCCESS;

error:
    vt->DeleteProgram(program_id);
    renderer->program_id = 0;
    return VLC_EGENERIC;
}

void
vlc_gl_renderer_Delete(struct vlc_gl_renderer *renderer)
{
    struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;

    vt->DeleteBuffers(1, &renderer->vertex_buffer_object);
    vt->DeleteBuffers(1, &renderer->index_buffer_object);
    vt->DeleteBuffers(interop->tex_count, renderer->texture_buffer_object);

    if (!interop->handle_texs_gen)
        vt->DeleteTextures(interop->tex_count, renderer->textures);

    vlc_gl_interop_Delete(interop);
    if (renderer->program_id != 0)
        renderer->vt->DeleteProgram(renderer->program_id);

#ifdef HAVE_LIBPLACEBO
    FREENULL(renderer->uloc.pl_vars);
    if (renderer->pl_ctx)
        pl_context_destroy(&renderer->pl_ctx);
#endif

    free(renderer);
}

struct vlc_gl_renderer *
vlc_gl_renderer_New(vlc_gl_t *gl, const opengl_vtable_t *vt,
                    vlc_video_context *context, const video_format_t *fmt,
                    bool supports_npot, bool b_dump_shaders)
{
    struct vlc_gl_renderer *renderer = calloc(1, sizeof(*renderer));
    if (!renderer)
        return NULL;

    struct vlc_gl_interop *interop =
        vlc_gl_interop_New(gl, vt, context, fmt, false);
    if (!interop)
    {
        free(renderer);
        return NULL;
    }

    renderer->interop = interop;

    renderer->gl = gl;
    renderer->vt = vt;
    renderer->b_dump_shaders = b_dump_shaders;
#if defined(USE_OPENGL_ES2)
    renderer->glsl_version = 100;
    renderer->glsl_precision_header = "precision highp float;\n";
#else
    renderer->glsl_version = 120;
    renderer->glsl_precision_header = "";
#endif

#ifdef HAVE_LIBPLACEBO
    // Create the main libplacebo context
    renderer->pl_ctx = vlc_placebo_Create(VLC_OBJECT(gl));
    if (renderer->pl_ctx) {
#   if PL_API_VER >= 20
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL);
#   elif PL_API_VER >= 6
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL, 0);
#   else
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL, 0, 0);
#   endif
    }
#endif

    int ret = opengl_link_program(renderer);
    if (ret != VLC_SUCCESS)
    {
        vlc_gl_renderer_Delete(renderer);
        return NULL;
    }

    getOrientationTransformMatrix(interop->fmt.orientation,
                                  renderer->var.OrientationMatrix);
    getViewpointMatrixes(renderer, interop->fmt.projection_mode);

    /* Update the fmt to main program one */
    renderer->fmt = interop->fmt;
    /* The orientation is handled by the orientation matrix */
    renderer->fmt.orientation = fmt->orientation;

    /* Texture size */
    for (unsigned j = 0; j < interop->tex_count; j++) {
        const GLsizei w = renderer->fmt.i_visible_width  * interop->texs[j].w.num
                        / interop->texs[j].w.den;
        const GLsizei h = renderer->fmt.i_visible_height * interop->texs[j].h.num
                        / interop->texs[j].h.den;
        if (supports_npot) {
            renderer->tex_width[j]  = w;
            renderer->tex_height[j] = h;
        } else {
            renderer->tex_width[j]  = vlc_align_pot(w);
            renderer->tex_height[j] = vlc_align_pot(h);
        }
    }

    if (!interop->handle_texs_gen)
    {
        ret = vlc_gl_interop_GenerateTextures(interop, renderer->tex_width,
                                              renderer->tex_height,
                                              renderer->textures);
        if (ret != VLC_SUCCESS)
        {
            vlc_gl_renderer_Delete(renderer);
            return NULL;
        }
    }

    /* */
    vt->Disable(GL_BLEND);
    vt->Disable(GL_DEPTH_TEST);
    vt->DepthMask(GL_FALSE);
    vt->Enable(GL_CULL_FACE);
    vt->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    vt->Clear(GL_COLOR_BUFFER_BIT);

    vt->GenBuffers(1, &renderer->vertex_buffer_object);
    vt->GenBuffers(1, &renderer->index_buffer_object);
    vt->GenBuffers(interop->tex_count, renderer->texture_buffer_object);

    return renderer;
}

static void UpdateZ(struct vlc_gl_renderer *renderer)
{
    /* Do trigonometry to calculate the minimal z value
     * that will allow us to zoom out without seeing the outside of the
     * sphere (black borders). */
    float tan_fovx_2 = tanf(renderer->f_fovx / 2);
    float tan_fovy_2 = tanf(renderer->f_fovy / 2);
    float z_min = - SPHERE_RADIUS / sinf(atanf(sqrtf(
                    tan_fovx_2 * tan_fovx_2 + tan_fovy_2 * tan_fovy_2)));

    /* The FOV value above which z is dynamically calculated. */
    const float z_thresh = 90.f;

    if (renderer->f_fovx <= z_thresh * M_PI / 180)
        renderer->f_z = 0;
    else
    {
        float f = z_min / ((FIELD_OF_VIEW_DEGREES_MAX - z_thresh) * M_PI / 180);
        renderer->f_z = f * renderer->f_fovx - f * z_thresh * M_PI / 180;
        if (renderer->f_z < z_min)
            renderer->f_z = z_min;
    }
}

static void UpdateFOVy(struct vlc_gl_renderer *renderer)
{
    renderer->f_fovy = 2 * atanf(tanf(renderer->f_fovx / 2) / renderer->f_sar);
}

int
vlc_gl_renderer_SetViewpoint(struct vlc_gl_renderer *renderer,
                             const vlc_viewpoint_t *p_vp)
{
    if (p_vp->fov > FIELD_OF_VIEW_DEGREES_MAX
            || p_vp->fov < FIELD_OF_VIEW_DEGREES_MIN)
        return VLC_EBADVAR;

    // Convert degree into radian
    float f_fovx = p_vp->fov * (float)M_PI / 180.f;

    /* vgl->vp needs to be converted into world transform */
    vlc_viewpoint_reverse(&renderer->vp, p_vp);

    if (fabsf(f_fovx - renderer->f_fovx) >= 0.001f)
    {
        /* FOVx has changed. */
        renderer->f_fovx = f_fovx;
        UpdateFOVy(renderer);
        UpdateZ(renderer);
    }
    getViewpointMatrixes(renderer, renderer->fmt.projection_mode);

    return VLC_SUCCESS;
}

void
vlc_gl_renderer_SetWindowAspectRatio(struct vlc_gl_renderer *renderer,
                                     float f_sar)
{
    /* Each time the window size changes, we must recompute the minimum zoom
     * since the aspect ration changes.
     * We must also set the new current zoom value. */
    renderer->f_sar = f_sar;
    UpdateFOVy(renderer);
    UpdateZ(renderer);
    getViewpointMatrixes(renderer, renderer->fmt.projection_mode);
}

int
vlc_gl_renderer_Prepare(struct vlc_gl_renderer *renderer, picture_t *picture)
{
    const struct vlc_gl_interop *interop = renderer->interop;
    /* Update the texture */
    return interop->ops->update_textures(interop, renderer->textures,
                                         renderer->tex_width,
                                         renderer->tex_height, picture,
                                         NULL);
}

static int BuildSphere(unsigned nbPlanes,
                        GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                        GLushort **indices, unsigned *nbIndices,
                        const float *left, const float *top,
                        const float *right, const float *bottom)
{
    unsigned nbLatBands = 128;
    unsigned nbLonBands = 128;

    *nbVertices = (nbLatBands + 1) * (nbLonBands + 1);
    *nbIndices = nbLatBands * nbLonBands * 3 * 2;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(nbPlanes * *nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    for (unsigned lat = 0; lat <= nbLatBands; lat++) {
        float theta = lat * (float) M_PI / nbLatBands;
        float sinTheta, cosTheta;

        sincosf(theta, &sinTheta, &cosTheta);

        for (unsigned lon = 0; lon <= nbLonBands; lon++) {
            float phi = lon * 2 * (float) M_PI / nbLonBands;
            float sinPhi, cosPhi;

            sincosf(phi, &sinPhi, &cosPhi);

            float x = cosPhi * sinTheta;
            float y = cosTheta;
            float z = sinPhi * sinTheta;

            unsigned off1 = (lat * (nbLonBands + 1) + lon) * 3;
            (*vertexCoord)[off1] = SPHERE_RADIUS * x;
            (*vertexCoord)[off1 + 1] = SPHERE_RADIUS * y;
            (*vertexCoord)[off1 + 2] = SPHERE_RADIUS * z;

            for (unsigned p = 0; p < nbPlanes; ++p)
            {
                unsigned off2 = (p * (nbLatBands + 1) * (nbLonBands + 1)
                                + lat * (nbLonBands + 1) + lon) * 2;
                float width = right[p] - left[p];
                float height = bottom[p] - top[p];
                float u = (float)lon / nbLonBands * width;
                float v = (float)lat / nbLatBands * height;
                (*textureCoord)[off2] = u;
                (*textureCoord)[off2 + 1] = v;
            }
        }
    }

    for (unsigned lat = 0; lat < nbLatBands; lat++) {
        for (unsigned lon = 0; lon < nbLonBands; lon++) {
            unsigned first = (lat * (nbLonBands + 1)) + lon;
            unsigned second = first + nbLonBands + 1;

            unsigned off = (lat * nbLatBands + lon) * 3 * 2;

            (*indices)[off] = first;
            (*indices)[off + 1] = second;
            (*indices)[off + 2] = first + 1;

            (*indices)[off + 3] = second;
            (*indices)[off + 4] = second + 1;
            (*indices)[off + 5] = first + 1;
        }
    }

    return VLC_SUCCESS;
}


static int BuildCube(unsigned nbPlanes,
                     float padW, float padH,
                     GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                     GLushort **indices, unsigned *nbIndices,
                     const float *left, const float *top,
                     const float *right, const float *bottom)
{
    *nbVertices = 4 * 6;
    *nbIndices = 6 * 6;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(nbPlanes * *nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    static const GLfloat coord[] = {
        -1.0,    1.0,    -1.0f, // front
        -1.0,    -1.0,   -1.0f,
        1.0,     1.0,    -1.0f,
        1.0,     -1.0,   -1.0f,

        -1.0,    1.0,    1.0f, // back
        -1.0,    -1.0,   1.0f,
        1.0,     1.0,    1.0f,
        1.0,     -1.0,   1.0f,

        -1.0,    1.0,    -1.0f, // left
        -1.0,    -1.0,   -1.0f,
        -1.0,     1.0,    1.0f,
        -1.0,     -1.0,   1.0f,

        1.0f,    1.0,    -1.0f, // right
        1.0f,   -1.0,    -1.0f,
        1.0f,   1.0,     1.0f,
        1.0f,   -1.0,    1.0f,

        -1.0,    -1.0,    1.0f, // bottom
        -1.0,    -1.0,   -1.0f,
        1.0,     -1.0,    1.0f,
        1.0,     -1.0,   -1.0f,

        -1.0,    1.0,    1.0f, // top
        -1.0,    1.0,   -1.0f,
        1.0,     1.0,    1.0f,
        1.0,     1.0,   -1.0f,
    };

    memcpy(*vertexCoord, coord, *nbVertices * 3 * sizeof(GLfloat));

    for (unsigned p = 0; p < nbPlanes; ++p)
    {
        float width = right[p] - left[p];
        float height = bottom[p] - top[p];

        float col[] = {left[p],
                       left[p] + width * 1.f/3,
                       left[p] + width * 2.f/3,
                       left[p] + width};

        float row[] = {top[p],
                       top[p] + height * 1.f/2,
                       top[p] + height};

        const GLfloat tex[] = {
            col[1] + padW, row[1] + padH, // front
            col[1] + padW, row[2] - padH,
            col[2] - padW, row[1] + padH,
            col[2] - padW, row[2] - padH,

            col[3] - padW, row[1] + padH, // back
            col[3] - padW, row[2] - padH,
            col[2] + padW, row[1] + padH,
            col[2] + padW, row[2] - padH,

            col[2] - padW, row[0] + padH, // left
            col[2] - padW, row[1] - padH,
            col[1] + padW, row[0] + padH,
            col[1] + padW, row[1] - padH,

            col[0] + padW, row[0] + padH, // right
            col[0] + padW, row[1] - padH,
            col[1] - padW, row[0] + padH,
            col[1] - padW, row[1] - padH,

            col[0] + padW, row[2] - padH, // bottom
            col[0] + padW, row[1] + padH,
            col[1] - padW, row[2] - padH,
            col[1] - padW, row[1] + padH,

            col[2] + padW, row[0] + padH, // top
            col[2] + padW, row[1] - padH,
            col[3] - padW, row[0] + padH,
            col[3] - padW, row[1] - padH,
        };

        memcpy(*textureCoord + p * *nbVertices * 2, tex,
               *nbVertices * 2 * sizeof(GLfloat));
    }

    const GLushort ind[] = {
        0, 1, 2,       2, 1, 3, // front
        6, 7, 4,       4, 7, 5, // back
        10, 11, 8,     8, 11, 9, // left
        12, 13, 14,    14, 13, 15, // right
        18, 19, 16,    16, 19, 17, // bottom
        20, 21, 22,    22, 21, 23, // top
    };

    memcpy(*indices, ind, *nbIndices * sizeof(GLushort));

    return VLC_SUCCESS;
}

static int BuildRectangle(unsigned nbPlanes,
                          GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                          GLushort **indices, unsigned *nbIndices,
                          const float *left, const float *top,
                          const float *right, const float *bottom)
{
    *nbVertices = 4;
    *nbIndices = 6;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(nbPlanes * *nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    static const GLfloat coord[] = {
       -1.0,    1.0,    -1.0f,
       -1.0,    -1.0,   -1.0f,
       1.0,     1.0,    -1.0f,
       1.0,     -1.0,   -1.0f
    };

    memcpy(*vertexCoord, coord, *nbVertices * 3 * sizeof(GLfloat));

    for (unsigned p = 0; p < nbPlanes; ++p)
    {
        const GLfloat tex[] = {
            left[p],  top[p],
            left[p],  bottom[p],
            right[p], top[p],
            right[p], bottom[p]
        };

        memcpy(*textureCoord + p * *nbVertices * 2, tex,
               *nbVertices * 2 * sizeof(GLfloat));
    }

    const GLushort ind[] = {
        0, 1, 2,
        2, 1, 3
    };

    memcpy(*indices, ind, *nbIndices * sizeof(GLushort));

    return VLC_SUCCESS;
}

static int SetupCoords(struct vlc_gl_renderer *renderer,
                       const float *left, const float *top,
                       const float *right, const float *bottom)
{
    const struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;

    GLfloat *vertexCoord, *textureCoord;
    GLushort *indices;
    unsigned nbVertices, nbIndices;

    int i_ret;
    switch (renderer->fmt.projection_mode)
    {
    case PROJECTION_MODE_RECTANGULAR:
        i_ret = BuildRectangle(interop->tex_count,
                               &vertexCoord, &textureCoord, &nbVertices,
                               &indices, &nbIndices,
                               left, top, right, bottom);
        break;
    case PROJECTION_MODE_EQUIRECTANGULAR:
        i_ret = BuildSphere(interop->tex_count,
                            &vertexCoord, &textureCoord, &nbVertices,
                            &indices, &nbIndices,
                            left, top, right, bottom);
        break;
    case PROJECTION_MODE_CUBEMAP_LAYOUT_STANDARD:
        i_ret = BuildCube(interop->tex_count,
                          (float)renderer->fmt.i_cubemap_padding / renderer->fmt.i_width,
                          (float)renderer->fmt.i_cubemap_padding / renderer->fmt.i_height,
                          &vertexCoord, &textureCoord, &nbVertices,
                          &indices, &nbIndices,
                          left, top, right, bottom);
        break;
    default:
        i_ret = VLC_EGENERIC;
        break;
    }

    if (i_ret != VLC_SUCCESS)
        return i_ret;

    for (unsigned j = 0; j < interop->tex_count; j++)
    {
        vt->BindBuffer(GL_ARRAY_BUFFER, renderer->texture_buffer_object[j]);
        vt->BufferData(GL_ARRAY_BUFFER, nbVertices * 2 * sizeof(GLfloat),
                       textureCoord + j * nbVertices * 2, GL_STATIC_DRAW);
    }

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer_object);
    vt->BufferData(GL_ARRAY_BUFFER, nbVertices * 3 * sizeof(GLfloat),
                   vertexCoord, GL_STATIC_DRAW);

    vt->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->index_buffer_object);
    vt->BufferData(GL_ELEMENT_ARRAY_BUFFER, nbIndices * sizeof(GLushort),
                   indices, GL_STATIC_DRAW);

    free(textureCoord);
    free(vertexCoord);
    free(indices);

    renderer->nb_indices = nbIndices;

    return VLC_SUCCESS;
}

static void DrawWithShaders(struct vlc_gl_renderer *renderer)
{
    const struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;
    renderer->pf_prepare_shader(renderer, renderer->tex_width,
                                renderer->tex_height, 1.0f);

    for (unsigned j = 0; j < interop->tex_count; j++) {
        assert(renderer->textures[j] != 0);
        vt->ActiveTexture(GL_TEXTURE0+j);
        vt->BindTexture(interop->tex_target, renderer->textures[j]);

        vt->BindBuffer(GL_ARRAY_BUFFER, renderer->texture_buffer_object[j]);

        assert(renderer->aloc.MultiTexCoord[j] != -1);
        vt->EnableVertexAttribArray(renderer->aloc.MultiTexCoord[j]);
        vt->VertexAttribPointer(renderer->aloc.MultiTexCoord[j], 2,
                                GL_FLOAT, 0, 0, 0);
    }

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer_object);
    vt->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->index_buffer_object);
    vt->EnableVertexAttribArray(renderer->aloc.VertexPosition);
    vt->VertexAttribPointer(renderer->aloc.VertexPosition, 3, GL_FLOAT, 0, 0, 0);

    const GLfloat *tm = NULL;
    if (interop->ops && interop->ops->get_transform_matrix)
        tm = interop->ops->get_transform_matrix(interop);
    if (!tm)
        tm = identity;

    vt->UniformMatrix4fv(renderer->uloc.TransformMatrix, 1, GL_FALSE, tm);

    vt->UniformMatrix4fv(renderer->uloc.OrientationMatrix, 1, GL_FALSE,
                         renderer->var.OrientationMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ProjectionMatrix, 1, GL_FALSE,
                         renderer->var.ProjectionMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ViewMatrix, 1, GL_FALSE,
                         renderer->var.ViewMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ZoomMatrix, 1, GL_FALSE,
                         renderer->var.ZoomMatrix);

    vt->DrawElements(GL_TRIANGLES, renderer->nb_indices, GL_UNSIGNED_SHORT, 0);
}


static void GetTextureCropParamsForStereo(unsigned i_nbTextures,
                                          const float *stereoCoefs,
                                          const float *stereoOffsets,
                                          float *left, float *top,
                                          float *right, float *bottom)
{
    for (unsigned i = 0; i < i_nbTextures; ++i)
    {
        float f_2eyesWidth = right[i] - left[i];
        left[i] = left[i] + f_2eyesWidth * stereoOffsets[0];
        right[i] = left[i] + f_2eyesWidth * stereoCoefs[0];

        float f_2eyesHeight = bottom[i] - top[i];
        top[i] = top[i] + f_2eyesHeight * stereoOffsets[1];
        bottom[i] = top[i] + f_2eyesHeight * stereoCoefs[1];
    }
}

static void TextureCropForStereo(struct vlc_gl_renderer *renderer,
                                 float *left, float *top,
                                 float *right, float *bottom)
{
    const struct vlc_gl_interop *interop = renderer->interop;

    float stereoCoefs[2];
    float stereoOffsets[2];

    switch (renderer->fmt.multiview_mode)
    {
    case MULTIVIEW_STEREO_TB:
        // Display only the left eye.
        stereoCoefs[0] = 1; stereoCoefs[1] = 0.5;
        stereoOffsets[0] = 0; stereoOffsets[1] = 0;
        GetTextureCropParamsForStereo(interop->tex_count,
                                      stereoCoefs, stereoOffsets,
                                      left, top, right, bottom);
        break;
    case MULTIVIEW_STEREO_SBS:
        // Display only the left eye.
        stereoCoefs[0] = 0.5; stereoCoefs[1] = 1;
        stereoOffsets[0] = 0; stereoOffsets[1] = 0;
        GetTextureCropParamsForStereo(interop->tex_count,
                                      stereoCoefs, stereoOffsets,
                                      left, top, right, bottom);
        break;
    default:
        break;
    }
}

int
vlc_gl_renderer_Draw(struct vlc_gl_renderer *renderer,
                     const video_format_t *source)
{
    const opengl_vtable_t *vt = renderer->vt;

    vt->Clear(GL_COLOR_BUFFER_BIT);

    vt->UseProgram(renderer->program_id);

    if (source->i_x_offset != renderer->last_source.i_x_offset
     || source->i_y_offset != renderer->last_source.i_y_offset
     || source->i_visible_width != renderer->last_source.i_visible_width
     || source->i_visible_height != renderer->last_source.i_visible_height)
    {
        float left[PICTURE_PLANE_MAX];
        float top[PICTURE_PLANE_MAX];
        float right[PICTURE_PLANE_MAX];
        float bottom[PICTURE_PLANE_MAX];
        const struct vlc_gl_interop *interop = renderer->interop;
        for (unsigned j = 0; j < interop->tex_count; j++)
        {
            float scale_w = (float)interop->texs[j].w.num / interop->texs[j].w.den
                          / renderer->tex_width[j];
            float scale_h = (float)interop->texs[j].h.num / interop->texs[j].h.den
                          / renderer->tex_height[j];

            /* Warning: if NPOT is not supported a larger texture is
               allocated. This will cause right and bottom coordinates to
               land on the edge of two texels with the texels to the
               right/bottom uninitialized by the call to
               glTexSubImage2D. This might cause a green line to appear on
               the right/bottom of the display.
               There are two possible solutions:
               - Manually mirror the edges of the texture.
               - Add a "-1" when computing right and bottom, however the
               last row/column might not be displayed at all.
            */
            left[j]   = (source->i_x_offset +                       0 ) * scale_w;
            top[j]    = (source->i_y_offset +                       0 ) * scale_h;
            right[j]  = (source->i_x_offset + source->i_visible_width ) * scale_w;
            bottom[j] = (source->i_y_offset + source->i_visible_height) * scale_h;
        }

        TextureCropForStereo(renderer, left, top, right, bottom);
        int ret = SetupCoords(renderer, left, top, right, bottom);
        if (ret != VLC_SUCCESS)
            return ret;

        renderer->last_source.i_x_offset = source->i_x_offset;
        renderer->last_source.i_y_offset = source->i_y_offset;
        renderer->last_source.i_visible_width = source->i_visible_width;
        renderer->last_source.i_visible_height = source->i_visible_height;
    }
    DrawWithShaders(renderer);

    return VLC_SUCCESS;
}
