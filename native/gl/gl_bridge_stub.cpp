#include "gl_bridge.hpp"

namespace codboz {

GlBridge::~GlBridge() = default;

bool GlBridge::init(void *) {
    return false;
}

bool GlBridge::attachSurface(void *) {
    return false;
}

void GlBridge::suspendSurface() {}

void GlBridge::swap() {}

std::string GlBridge::diagnostics() const {
    return "bounded GLES2 profile; host renderer unavailable";
}

void GlBridge::registerHandlers(std::unordered_map<std::string, Handler> &,
                                std::unordered_map<GuestAddr, Handler> &, GuestMemory &) {}

void GlBridge::forwardSimple(std::unordered_map<std::string, Handler> &, GuestMemory &) {}

} // namespace codboz
