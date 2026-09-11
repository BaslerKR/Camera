#include "CameraGrabDiagnostics.h"

#include <cassert>

int main()
{
    assert(!CameraGrabDiagnostics::shouldReportFailure(0U));
    assert(CameraGrabDiagnostics::shouldReportFailure(1U));
    assert(!CameraGrabDiagnostics::shouldReportFailure(2U));
    assert(!CameraGrabDiagnostics::shouldReportFailure(99U));
    assert(CameraGrabDiagnostics::shouldReportFailure(100U));
    assert(!CameraGrabDiagnostics::shouldReportFailure(101U));
    assert(CameraGrabDiagnostics::shouldReportFailure(200U));
    return 0;
}
