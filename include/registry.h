#pragma once

#include "service/echo_service.h"
#include "service/app_service.h"
#include "service/completions_service.h"
#include "service/course_service.h"
#include "service/ecard_service.h"
#include "service/electricity_service.h"
#include "service/portal_service.h"
#include "service/service.h"
#include "service/sso_service.h"
#include "model/constants.h"

#include <boost/describe/class.hpp>

#include <string_view>

namespace zzu_assistant {
    class Registry final {
    public:
        int dispatch(ServiceContext &context, Arguments arguments);

        void print_usage(std::ostream &output, std::string_view executable_name,
                         bool color_enabled) const;

    private:
        // Each reflected member is exposed as a CLI command using its member name.
        // Add a Service-derived member here and to BOOST_DESCRIBE_CLASS to register it.
        services::EchoService echo;
        services::ElectricityService electricity;
        services::EcardService ecard;
        services::CourseService course;
        services::SSOService sso;
        services::PortalService portal;
        services::AppService app;
        services::CompletionsService _completions;

        BOOST_DESCRIBE_CLASS(Registry, (), (), (),
                             (echo, electricity, ecard, course, sso, portal, app,
                              _completions))
    };
} // namespace zzu_assistant
