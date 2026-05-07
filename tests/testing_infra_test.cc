#include <gtest/gtest.h>

TEST(ToolingSmokeTest, GTestIsWired) {
  int a = 14;
  int b = 28;
  int sum = a + b;
  EXPECT_EQ(sum, 42);
}
