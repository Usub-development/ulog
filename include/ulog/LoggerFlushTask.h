//
// Created by root on 10/28/25.
//

#ifndef LOGGERFLUSHTASK_H
#define LOGGERFLUSHTASK_H

#include "uvent/Uvent.h"
#include "Logger.h"

namespace usub::ulog
{
    uvent::task::Awaitable<void> logger_flush_task();
}

#endif //LOGGERFLUSHTASK_H
