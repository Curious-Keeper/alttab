#pragma once

#include "defines.hpp"
#include "logger.hpp"

struct StyleContext {
  size_t index;
  size_t count;
  size_t activeIndex;
  float rotation;
  float scale;
  float alpha;
  Vector2D mSize;
  Vector2D offset;
};

struct RenderData {
  bool visible;
  float z;
  float rotation;
  float scale;
  float alpha;
  CBox position;
};

struct MoveResult {
  bool changeMonitor = false;
  std::optional<size_t> index = std::nullopt;
};

class IStyle {
public:
  virtual ~IStyle() = default;
  virtual RenderData calculate(const StyleContext &ctx, const Vector2D &surfaceSize) const = 0;
  virtual MoveResult onMove(Direction dir, const size_t index, const size_t count) = 0;
};

class Carousel : public IStyle {
public:
  RenderData calculate(const StyleContext &ctx, const Vector2D &surfaceSize) const override;
  MoveResult onMove(Direction dir, const size_t index, const size_t count) override;
};

class Grid : public IStyle {
public:
  RenderData calculate(const StyleContext &ctx, const Vector2D &surfaceSize) const override;
  MoveResult onMove(Direction dir, const size_t index, const size_t count) override;

private:
  const int cols = 4;
};

class Slide : public IStyle {
public:
  RenderData calculate(const StyleContext &ctx, const Vector2D &surfaceSize) const override;
  MoveResult onMove(Direction dir, const size_t index, const size_t count) override;
};
