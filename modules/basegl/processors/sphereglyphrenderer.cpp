/*********************************************************************************
 *
 * Inviwo - Interactive Visualization Workshop
 *
 * Copyright (c) 2017 Inviwo Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************************/

#include <modules/basegl/processors/sphereglyphrenderer.h>
#include <modules/opengl/rendering/meshdrawergl.h>
#include <modules/opengl/shader/shaderutils.h>

namespace inviwo {

// The Class Identifier has to be globally unique. Use a reverse DNS naming scheme
const ProcessorInfo SphereGlyphRenderer::processorInfo_{
    "org.inviwo.SphereGlyphRenderer",  // Class identifier
    "Sphere Renderer",                 // Display name
    "Mesh Rendering",                  // Category
    CodeState::Stable,                 // Code state
    Tags::GL,                          // Tags
};
const ProcessorInfo SphereGlyphRenderer::getProcessorInfo() const { return processorInfo_; }

SphereGlyphRenderer::SphereGlyphRenderer()
    : Processor()
    , inport_("geometry")
    , imageInport_("imageInport")
    , outport_("image")
    , renderMode_("renderMode", "Render Mode",
                  {{"entireMesh", "Entire Mesh", RenderMode::EntireMesh},
                   {"pointsOnly", "Points Only", RenderMode::PointsOnly}})
    , clipping_("clipping", "Clipping")
    , clipMode_("clipMode", "Clip Mode",
                {{"none", "None", GlyphClippingMode::None},
                 {"hollow", "Hollow", GlyphClippingMode::Hollow},
                 {"solid", "Solid", GlyphClippingMode::Solid}},
                2, InvalidationLevel::InvalidResources)
    , clipShadingFactor_("clipShadingFactor", "Clip Surface Adjustment", 0.9f, 0.0f, 2.0f)
    , shadeClippedArea_("shadeClippedArea", "Shade Clipped Area", false,
                        InvalidationLevel::InvalidResources)
    , sphereProperties_("sphereProperties", "Sphere Properties")
    , overrideSphereRadius_("overrideSphereRadius", "Override Sphere Radius", false,
                          InvalidationLevel::InvalidResources)
    , customRadius_("customRadius", "Custom Radius", 0.05f, 0.00001f, 2.0f, 0.01f)
    , overrideSphereColor_("overrideSphereColor", "Override Sphere Color", false,
                      InvalidationLevel::InvalidResources)
    , customColor_("customColor", "Custom Color", vec4(0.7f, 0.7f, 0.7f, 1.0f), vec4(0.0f),
                   vec4(1.0f))

    , camera_("camera", "Camera")
    , lighting_("lighting", "Lighting", &camera_)
    , trackball_(&camera_)
    , shader_("sphereglyph.vert", "sphereglyph.geom", "sphereglyph.frag", false) {

    outport_.addResizeEventListener(&camera_);

    addPort(inport_);
    addPort(imageInport_);
    addPort(outport_);
    imageInport_.setOptional(true);

    customColor_.setSemantics(PropertySemantics::Color);

    clipping_.addProperty(clipMode_);
    clipping_.addProperty(clipShadingFactor_);
    clipping_.addProperty(shadeClippedArea_);

    sphereProperties_.addProperty(overrideSphereRadius_);
    sphereProperties_.addProperty(customRadius_);
    sphereProperties_.addProperty(overrideSphereColor_);
    sphereProperties_.addProperty(customColor_);

    addProperty(renderMode_);
    addProperty(sphereProperties_);
    addProperty(clipping_);
    
    addProperty(camera_);
    addProperty(lighting_);
    addProperty(trackball_);

    clipMode_.onChange([&]() {
        clipShadingFactor_.setReadOnly(clipMode_.get() == GlyphClippingMode::None);
        shadeClippedArea_.setReadOnly(clipMode_.get() == GlyphClippingMode::None);
    });

    shader_.onReload([this]() { invalidate(InvalidationLevel::InvalidResources); });
    shader_.getVertexShaderObject()->addShaderExtension("GL_EXT_geometry_shader4", true);
}

void SphereGlyphRenderer::process() {
    if (imageInport_.isConnected()) {
        utilgl::activateTargetAndCopySource(outport_, imageInport_, ImageType::ColorDepthPicking);
    } else {
        utilgl::activateAndClearTarget(outport_, ImageType::ColorDepthPicking);
    }

    shader_.activate();

    utilgl::setUniforms(shader_, camera_, lighting_, customColor_, customRadius_,
                        clipShadingFactor_);
    shader_.setUniform("viewport", vec4(0.0f, 0.0f, 2.0f / outport_.getDimensions().x,
                                        2.0f / outport_.getDimensions().y));
    drawMeshes();

    shader_.deactivate();
    utilgl::deactivateCurrentTarget();
}

void SphereGlyphRenderer::initializeResources() {
    utilgl::addShaderDefines(shader_, lighting_);

    if (overrideSphereRadius_.get()) {
        shader_.getVertexShaderObject()->addShaderDefine("UNIFORM_RADIUS");
    } else {
        shader_.getVertexShaderObject()->removeShaderDefine("UNIFORM_RADIUS");
    }

    if (overrideSphereColor_.get()) {
        shader_.getVertexShaderObject()->addShaderDefine("UNIFORM_COLOR");
    } else {
        shader_.getVertexShaderObject()->removeShaderDefine("UNIFORM_COLOR");
    }

    if (shadeClippedArea_.get()) {
        shader_.getFragmentShaderObject()->addShaderDefine("SHADE_CLIPPED_AREA");
    } else {
        shader_.getFragmentShaderObject()->removeShaderDefine("SHADE_CLIPPED_AREA");
    }

    std::string clipModeKey =
        "APPLY_GLYPH_CLIPPING(coord, intersection, mvpTranspose, srcColor, dstColor, dstDepth)";
    std::string value = "";

    switch (clipMode_.get()) {
        case GlyphClippingMode::None:
            value = "discard";
            break;
        case GlyphClippingMode::Hollow:
            value = "clipToHollowGlyph(intersection, mvpTranspose, srcColor, dstColor, dstDepth)";
            break;
        case GlyphClippingMode::Solid:
        default:
            value = "clipToSolid(coord, srcColor, dstColor, dstDepth)";
            break;
    }
    shader_.getFragmentShaderObject()->addShaderDefine(clipModeKey, value);

    shader_.build();
}

void SphereGlyphRenderer::drawMeshes() {
    switch (renderMode_.get()) {
        case RenderMode::PointsOnly:
            // render only index buffers marked as points (or the entire mesh if none exists)
            for (const auto& elem : inport_) {
                MeshDrawerGL::DrawObject drawer(elem->getRepresentation<MeshGL>(),
                                                elem->getDefaultMeshInfo());
                utilgl::setShaderUniforms(shader_, *elem, "geometry");
                if (elem->getNumberOfIndicies() > 0) {
                    for (size_t i = 0; i < elem->getNumberOfIndicies(); ++i) {
                        auto meshinfo = elem->getIndexMeshInfo(i);
                        if ((meshinfo.dt == DrawType::Points) ||
                            (meshinfo.dt == DrawType::NotSpecified)) {
                            drawer.draw(MeshDrawerGL::DrawMode::Points, i);
                        }
                    }
                } else {
                    // no index buffers, check mesh default draw type
                    auto drawtype = elem->getDefaultMeshInfo().dt;
                    if ((drawtype == DrawType::Points) || (drawtype == DrawType::NotSpecified)) {
                        drawer.draw(MeshDrawerGL::DrawMode::Points);
                    }
                }
            }
            break;
        case RenderMode::EntireMesh:
        default:
            // render all parts of the input meshes as points
            for (const auto& elem : inport_) {
                MeshDrawerGL::DrawObject drawer(elem->getRepresentation<MeshGL>(),
                                                elem->getDefaultMeshInfo());
                utilgl::setShaderUniforms(shader_, *elem, "geometry");
                drawer.draw(MeshDrawerGL::DrawMode::Points);
            }
            break;
    }
}

}  // namespace inviwo
