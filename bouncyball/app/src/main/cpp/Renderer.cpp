/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Renderer.h"

#define LOG_TAG "Renderer"

#include <vector>

#include <GLES2/gl2.h>

#include <android/native_window.h>

#include "swappy-utils/Log.h"
#include "swappy-utils/Settings.h"
#include "swappy-utils/Trace.h"

#include "swappy/Swappy.h"

#include "Circle.h"

using namespace std::chrono_literals;

Renderer *Renderer::getInstance() {
    static std::unique_ptr<Renderer> sRenderer = std::make_unique<Renderer>(ConstructorTag{});
    return sRenderer.get();
}

void Renderer::setWindow(ANativeWindow *window, int32_t width, int32_t height) {
    mWorkerThread.run([=](ThreadState *threadState) {
        threadState->clearSurface();

        if (!window) return;

        threadState->surface =
            eglCreateWindowSurface(threadState->display, threadState->config, window, NULL);
        ANativeWindow_release(window);
        if (!threadState->makeCurrent(threadState->surface)) {
            ALOGE("Unable to eglMakeCurrent");
            threadState->surface = EGL_NO_SURFACE;
            return;
        }

        threadState->width = width;
        threadState->height = height;
    });
}

void Renderer::start() {
    mWorkerThread.run([this](ThreadState *threadState) {
        threadState->isStarted = true;
        requestDraw();
    });
}

void Renderer::stop() {
    mWorkerThread.run([=](ThreadState *threadState) { threadState->isStarted = false; });
}

void Renderer::requestDraw() {
    mWorkerThread.run([=](ThreadState *threadState) { if (threadState->isStarted) draw(threadState); });
}

Renderer::ThreadState::ThreadState() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, 0, 0);

    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    eglChooseConfig(display, configAttributes, nullptr, 0, &numConfigs);
    std::vector<EGLConfig> supportedConfigs(static_cast<size_t>(numConfigs));
    eglChooseConfig(display, configAttributes, supportedConfigs.data(), numConfigs, &numConfigs);

    // Choose a config, either a match if possible or the first config otherwise

    const auto configMatches = [&](EGLConfig config) {
        if (!configHasAttribute(EGL_RED_SIZE, 8)) return false;
        if (!configHasAttribute(EGL_GREEN_SIZE, 8)) return false;
        if (!configHasAttribute(EGL_BLUE_SIZE, 8)) return false;
        return configHasAttribute(EGL_DEPTH_SIZE, 0);
    };

    const auto configIter = std::find_if(supportedConfigs.cbegin(), supportedConfigs.cend(),
                                         configMatches);

    config = (configIter != supportedConfigs.cend()) ? *configIter : supportedConfigs[0];

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    context = eglCreateContext(display, config, nullptr, contextAttributes);

    glEnable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}

Renderer::ThreadState::~ThreadState() {
    clearSurface();
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
}

void Renderer::ThreadState::onSettingsChanged(const Settings * settings) {
    refreshPeriod = settings->getRefreshPeriod();
    swapInterval = settings->getSwapInterval();
}

void Renderer::ThreadState::clearSurface() {
    if (surface == EGL_NO_SURFACE) return;

    makeCurrent(EGL_NO_SURFACE);
    eglDestroySurface(display, surface);
    surface = EGL_NO_SURFACE;
}

bool Renderer::ThreadState::configHasAttribute(EGLint attribute, EGLint value) {
    EGLint outValue = 0;
    EGLBoolean result = eglGetConfigAttrib(display, config, attribute, &outValue);
    return result && (outValue == value);
}

EGLBoolean Renderer::ThreadState::makeCurrent(EGLSurface surface) {
    return eglMakeCurrent(display, surface, surface, context);
}

void Renderer::draw(ThreadState *threadState) {
    // Don't render if we have no surface
    if (threadState->surface == EGL_NO_SURFACE) {
        // Sleep a bit so we don't churn too fast
        std::this_thread::sleep_for(50ms);
        requestDraw();
        return;
    }

    const float deltaSeconds = (threadState->refreshPeriod * threadState->swapInterval).count() / 1e9f;

    threadState->x += threadState->velocity * deltaSeconds;

    if (threadState->x > 0.8f) {
        threadState->velocity *= -1.0f;
        threadState->x = 1.6f - threadState->x;
    } else if (threadState->x < -0.8f) {
        threadState->velocity *= -1.0f;
        threadState->x = -1.6f - threadState->x;
    }

    // Just fill the screen with a color.
    glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    const float aspectRatio = static_cast<float>(threadState->width) / threadState->height;

    const std::vector<Circle>
        circles = {{Circle::Color{0.0f, 1.0f, 1.0f}, 0.1f, threadState->x, 0.0f}};

    Circle::draw(aspectRatio, circles);

    Swappy::swap(threadState->display, threadState->surface);

    // If we're still started, request another frame
    requestDraw();
}