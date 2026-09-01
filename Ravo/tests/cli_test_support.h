#pragma once

#include <gtest/gtest.h>

#include "ravo/engine/engine.h"

namespace ravo
{

class CliTest : public ::testing::Test
{
protected:
    CliTest();
    ~CliTest() override;

    void SetUp() override;

    EngineFacade engine;
};

} // namespace ravo
