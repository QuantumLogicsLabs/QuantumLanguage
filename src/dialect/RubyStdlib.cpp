#include "Dialect.h"

#include <string>
#include <vector>

// Ruby's `Enumerable` mixin. Every method here is defined purely in terms of
// whatever `each` the including class provides, so it is synthesized as
// ordinary Ruby source lines — in the same subset the transpiler already
// understands — rather than as a special native feature. `include Enumerable`
// splices these in through the exact same path as a user-defined
// `module ... end`.
//
// Note: a non-local `return` inside a block is not available to the
// transpiled closures (a `return` there returns from the closure, not the
// enclosing method), so `first` uses a done-flag instead of an early return.
const std::vector<std::string> &rubyEnumerableModuleSource()
{
    static const std::vector<std::string> source = {
        "  def to_a",
        "    __r = []",
        "    self.each { |x| __r << x }",
        "    __r",
        "  end",
        "  def map",
        "    __r = []",
        "    self.each { |x| __r << yield(x) }",
        "    __r",
        "  end",
        "  def select",
        "    __r = []",
        "    self.each { |x| __r << x if yield(x) }",
        "    __r",
        "  end",
        "  def reject",
        "    __r = []",
        "    self.each { |x| __r << x unless yield(x) }",
        "    __r",
        "  end",
        "  def reduce(initial)",
        "    __acc = initial",
        "    self.each { |x| __acc = yield(__acc, x) }",
        "    __acc",
        "  end",
        "  def sum",
        "    __t = 0",
        "    self.each { |x| __t += x }",
        "    __t",
        "  end",
        "  def count",
        "    __n = 0",
        "    self.each { |x| __n += 1 }",
        "    __n",
        "  end",
        "  def include?(item)",
        "    __found = false",
        "    self.each { |x| __found = true if x == item }",
        "    __found",
        "  end",
        "  def min",
        "    __m = nil",
        "    self.each { |x| __m = x if __m == nil || x < __m }",
        "    __m",
        "  end",
        "  def max",
        "    __m = nil",
        "    self.each { |x| __m = x if __m == nil || x > __m }",
        "    __m",
        "  end",
        "  def sort",
        "    self.to_a.sort",
        "  end",
        "  def any?",
        "    __found = false",
        "    self.each { |x| __found = true if yield(x) }",
        "    __found",
        "  end",
        "  def all?",
        "    __r = true",
        "    self.each { |x| __r = false unless yield(x) }",
        "    __r",
        "  end",
        "  def none?",
        "    __found = false",
        "    self.each { |x| __found = true if yield(x) }",
        "    !__found",
        "  end",
        "  def first",
        "    __r = nil",
        "    __done = false",
        "    self.each do |x|",
        "      unless __done",
        "        __r = x",
        "        __done = true",
        "      end",
        "    end",
        "    __r",
        "  end",
    };
    return source;
}
