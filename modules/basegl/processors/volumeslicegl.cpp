/*********************************************************************************
 *
 * Inviwo - Interactive Visualization Workshop
 * Version 0.6b
 *
 * Copyright (c) 2013-2014 Inviwo Foundation
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
 * Main file author: Erik Sund�n
 *
 *********************************************************************************/

#include "volumeslicegl.h"
#include <modules/opengl/volume/volumegl.h>

namespace inviwo {

ProcessorClassName(VolumeSliceGL, "VolumeSliceGL");
ProcessorCategory(VolumeSliceGL, "Volume Operation");
ProcessorCodeState(VolumeSliceGL, CODE_STATE_STABLE);

VolumeSliceGL::VolumeSliceGL()
    : ProcessorGL()
    , inport_("volume.inport")
    , outport_("image.outport", COLOR_ONLY)
    , sliceAlongAxis_("sliceAxis", "Slice along axis")
    , rotationAroundAxis_("rotation", "Rotation around axis (degrees)")
    , flipHorizontal_("flipHorizontal", "Flip Horizontal View", false)
    , flipVertical_("flipVertical", "Flip Vertical View", false)
    , sliceNumber_("sliceNumber", "Slice Number", 4, 1, 8)
    , tfMappingEnabled_("tfMappingEnabled", "Enable Transfer Function", true)
    , transferFunction_("transferFunction", "Transfer function", TransferFunction(), &inport_)
    , shader_(NULL) {
      
    addPort(inport_);
    addPort(outport_);

    inport_.onChange(this, &VolumeSliceGL::updateMaxSliceNumber);
    sliceAlongAxis_.addOption("x", "X axis", CoordinateEnums::X);
    sliceAlongAxis_.addOption("y", "Y axis", CoordinateEnums::Y);
    sliceAlongAxis_.addOption("z", "Z axis", CoordinateEnums::Z);
    sliceAlongAxis_.set(CoordinateEnums::X);
    sliceAlongAxis_.setCurrentStateAsDefault();
    sliceAlongAxis_.onChange(this, &VolumeSliceGL::planeSettingsChanged);
    addProperty(sliceAlongAxis_);

    rotationAroundAxis_.addOption("0", "0", 0.f);
    rotationAroundAxis_.addOption("90", "90", glm::radians(90.f));
    rotationAroundAxis_.addOption("180", "180", glm::radians(180.f));
    rotationAroundAxis_.addOption("270", "270", glm::radians(270.f));
    rotationAroundAxis_.set(0.f);
    rotationAroundAxis_.setCurrentStateAsDefault();
    rotationAroundAxis_.onChange(this, &VolumeSliceGL::planeSettingsChanged);
    addProperty(rotationAroundAxis_);
    
    flipHorizontal_.onChange(this, &VolumeSliceGL::planeSettingsChanged);
    addProperty(flipHorizontal_);
    flipVertical_.onChange(this, &VolumeSliceGL::planeSettingsChanged);
    addProperty(flipVertical_);
    addProperty(sliceNumber_);
    tfMappingEnabled_.onChange(this, &VolumeSliceGL::tfMappingEnabledChanged);
    addProperty(tfMappingEnabled_);
    // Make sure that opacity does not affect the mapped color.
    if (transferFunction_.get().getNumDataPoints() > 0) {
        transferFunction_.get().getPoint(0)->setA(1.f);
    }
    transferFunction_.setCurrentStateAsDefault();
    addProperty(transferFunction_);
    volumeDimensions_ = uvec3(8);
    addInteractionHandler(new VolumeSliceGLInteractationHandler(this));
}

VolumeSliceGL::~VolumeSliceGL() {
    const std::vector<InteractionHandler*>& interactionHandlers = getInteractionHandlers();
    for(size_t i=0; i<interactionHandlers.size(); ++i) {
        InteractionHandler* handler = interactionHandlers[i];
        removeInteractionHandler(handler);
        delete handler;
    }
}

void VolumeSliceGL::initialize() {
    ProcessorGL::initialize();
    shader_ = new Shader("img_texturequad.vert", "volumeslice.frag", true);
    planeSettingsChanged();
    tfMappingEnabledChanged();
    updateMaxSliceNumber();
}

void VolumeSliceGL::deinitialize() {
    delete shader_;
    ProcessorGL::deinitialize();
}

void VolumeSliceGL::process() {
    if (volumeDimensions_ != inport_.getData()->getDimension()) {
        volumeDimensions_ = inport_.getData()->getDimension();
        updateMaxSliceNumber();
    }

    TextureUnit transFuncUnit;
    const Layer* tfLayer = transferFunction_.get().getData();
    const LayerGL* transferFunctionGL = tfLayer->getRepresentation<LayerGL>();
    transferFunctionGL->bindTexture(transFuncUnit.getEnum());
    TextureUnit volUnit;
    const VolumeGL* volumeGL = inport_.getData()->getRepresentation<VolumeGL>();
    volumeGL->bindTexture(volUnit.getEnum());
    activateAndClearTarget(outport_);
    shader_->activate();
    setGlobalShaderParameters(shader_);

    if (tfMappingEnabled_.get())
        shader_->setUniform("transferFunc_", transFuncUnit.getUnitNumber());

    shader_->setUniform("volume_", volUnit.getUnitNumber());
    volumeGL->setVolumeUniforms(inport_.getData(), shader_, "volumeParameters_");
    shader_->setUniform("dimension_", vec2(1.0f/outport_.getData()->getDimension().x, 1.0f/outport_.getData()->getDimension().y));
    float sliceNum = (static_cast<float>(sliceNumber_.get())-0.5f)/glm::max<float>(static_cast<float>(sliceNumber_.getMaxValue()), 1.f);
    shader_->setUniform("sliceNum_", sliceNum);
    renderImagePlaneRect();
    shader_->deactivate();
    deactivateCurrentTarget();
}

void VolumeSliceGL::shiftSlice(int shift){
    int newSlice = sliceNumber_.get()+shift;
    if(newSlice >= sliceNumber_.getMinValue() && newSlice <= sliceNumber_.getMaxValue())
        sliceNumber_.set(newSlice);
}

void VolumeSliceGL::planeSettingsChanged() {  
    std::string fH = (flipHorizontal_.get() ? "1.0-" : "");
    std::string fV = (flipVertical_.get() ? "1.0-" : "");
    // map (x, y, z) to volume texture space coordinates
    // Input:
    // z is the direction in which we slice
    // x is the horizontal direction and y is the vertical direction of the output image

    if (shader_) {
        switch (sliceAlongAxis_.get())
        {
            case CoordinateEnums::X:
                shader_->getFragmentShaderObject()->addShaderDefine("coordPlanePermute(x,y,z)", "z," + fV + "y,"  + fH +"x");
                break;
            case CoordinateEnums::Y:
                shader_->getFragmentShaderObject()->addShaderDefine("coordPlanePermute(x,y,z)", fH + "x,z," + fV + "y");
                break;
            case CoordinateEnums::Z:
                shader_->getFragmentShaderObject()->addShaderDefine("coordPlanePermute(x,y,z)", fH + "x," + fV + "y,z");
                break;
        }

        shader_->getFragmentShaderObject()->addShaderDefine("COORD_PLANE_PERMUTE");

        shader_->getFragmentShaderObject()->build();
        shader_->link();
        // Rotation
        float rotationAngle = rotationAroundAxis_.get();
        // Maintain clockwise rotation even if horizontal axis is flipped.
        if (flipHorizontal_.get()) {
            rotationAngle = -rotationAngle;
        }
        vec3 sliceAxis;
        switch (sliceAlongAxis_.get())
        {
        case CoordinateEnums::X:
            sliceAxis = vec3(1.f, 0.f, 0.f);
            break;
        case CoordinateEnums::Y:
            sliceAxis = vec3(0.f, 1.f, 0.f);
            break;
        case CoordinateEnums::Z:
            sliceAxis = vec3(0.f, 0.f, 1.f);
            break;
        }
        // Offset during rotation to rotate around the center point 
        vec3 rotationOffset = vec3(-0.5f)+0.5f*sliceAxis;
        mat4 rotationMat = glm::translate(glm::rotate(rotationAngle, sliceAxis), rotationOffset);
        shader_->activate();
        shader_->setUniform("sliceAxisRotationMatrix_", rotationMat);
        // Translate back after rotation
        shader_->setUniform("rotationOffset_", -rotationOffset);
        shader_->deactivate();
    }

    updateMaxSliceNumber();
}

void VolumeSliceGL::tfMappingEnabledChanged() {
    if (shader_) {
        if (tfMappingEnabled_.get()) {
            shader_->getFragmentShaderObject()->addShaderDefine("TF_MAPPING_ENABLED");
            transferFunction_.setVisible(true);
        }
        else {
            shader_->getFragmentShaderObject()->removeShaderDefine("TF_MAPPING_ENABLED");
            transferFunction_.setVisible(false);
        }
        planeSettingsChanged();
    }
}

void VolumeSliceGL::updateMaxSliceNumber() {
    if (!inport_.hasData()) {
        return;
    }
    uvec3 dims = inport_.getData()->getDimension();
    switch (sliceAlongAxis_.get())
    {
        case CoordinateEnums::X:
            if(dims.x!=sliceNumber_.getMaxValue()){
                sliceNumber_.setMaxValue(static_cast<int>(dims.x));
                sliceNumber_.set(static_cast<int>(dims.x)/2);
            }
            break;
        case CoordinateEnums::Y:
            if(dims.y!=sliceNumber_.getMaxValue()){
                sliceNumber_.setMaxValue(static_cast<int>(dims.y));
                sliceNumber_.set(static_cast<int>(dims.y)/2);
            }
            break;
        case CoordinateEnums::Z:
            if(dims.z!=sliceNumber_.getMaxValue()){
                sliceNumber_.setMaxValue(static_cast<int>(dims.z));
                sliceNumber_.set(static_cast<int>(dims.z)/2);
            }
            break;
    }
}

VolumeSliceGL::VolumeSliceGLInteractationHandler::VolumeSliceGLInteractationHandler(VolumeSliceGL* vs) 
    : InteractionHandler()
    , wheelEvent_(MouseEvent::MOUSE_BUTTON_NONE, InteractionEvent::MODIFIER_NONE)
    , upEvent_('W',InteractionEvent::MODIFIER_NONE, KeyboardEvent::KEY_STATE_PRESS)
    , downEvent_('S',InteractionEvent::MODIFIER_NONE, KeyboardEvent::KEY_STATE_PRESS)
    , slicer_(vs) {
}

void VolumeSliceGL::VolumeSliceGLInteractationHandler::invokeEvent(Event* event){
    GestureEvent* gestureEvent = dynamic_cast<GestureEvent*>(event);
    if (gestureEvent) {
        if(gestureEvent->type() == GestureEvent::PAN){
            if (gestureEvent->deltaPos().y < 0)
                slicer_->shiftSlice(1);
            else if (gestureEvent->deltaPos().y > 0)
                slicer_->shiftSlice(-1);
        }
        return;
    }

    KeyboardEvent* keyEvent = dynamic_cast<KeyboardEvent*>(event);
    if (keyEvent) {
        int button = keyEvent->button();
        KeyboardEvent::KeyState state = keyEvent->state();
        InteractionEvent::Modifier modifier = keyEvent->modifier();

        if (button == upEvent_.button()
            && modifier == upEvent_.modifier()
            && state == KeyboardEvent::KEY_STATE_PRESS)
            slicer_->shiftSlice(1);
        else if (button == downEvent_.button()
            && modifier == downEvent_.modifier()
            && state == KeyboardEvent::KEY_STATE_PRESS)
            slicer_->shiftSlice(-1);

        return;
    }

    MouseEvent* mouseEvent = dynamic_cast<MouseEvent*>(event);
    if (mouseEvent) {
        MouseEvent::MouseState state = mouseEvent->state();
        InteractionEvent::Modifier modifier = mouseEvent->modifier();

        if (modifier == wheelEvent_.modifier()
            && state == MouseEvent::MOUSE_STATE_WHEEL) {
                int steps = mouseEvent->wheelSteps();
                slicer_->shiftSlice(steps);
        }
        return;
    }
}

} // inviwo namespace
