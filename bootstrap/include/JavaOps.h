#pragma once
// Operations of the Java opcodes (Op::J_BIN / Op::J_UN), shared by the Java
// front-end (src/java/), which emits them, and the VM (src/vm/VmJava.cpp),
// which runs them.
//
// Runtime representation of Java values on the Quantum VM:
//   boolean                      bool
//   byte short char int long     int64 (QuantumValue::ofLong), kept wrapped
//                                to the type's width by these operations
//   float double                 double (a float is rounded to float)
//   String                       string (UTF-8; indices count UTF-16 units)
//   null                         nil
//   arrays                       array
//   objects                      instance of the class's QuantumClass
//   lambdas / method references  closure
//   boxed Integer                int64; Double/Float double; Boolean bool
//   boxed Long / Short / Byte     instance of java.lang.Long / Short / Byte
//                                (field "value"), so they stay distinct from
//                                Integer as on a JVM; valueOf caches -128..127
//   boxed Character              instance of java.lang.Character (field "value"),
//                                valueOf cached for 0..127
#include <cstdint>

namespace jop
{

enum Bin : int32_t
{
    IADD, ISUB, IMUL, IDIV, IREM, ISHL, ISHR, IUSHR, IAND, IOR, IXOR,
    LADD, LSUB, LMUL, LDIV, LREM, LSHL, LSHR, LUSHR, LAND, LOR, LXOR,
    FADD, FSUB, FMUL, FDIV, FREM,
    DADD, DSUB, DMUL, DDIV, DREM,
    LT, LE, GT, GE, EQ, NE, // numeric comparison (any mix of int64 / double)
    REFEQ, REFNE,           // == / != on references (strings compare by value)
    SCAT,                   // string + string
    CHARAT,                 // String.charAt(int): a UTF-16 unit (StringIndexOutOfBoundsException)
    BXOR,                   // boolean ^ (and & | on booleans, both sides evaluated)
    BAND,
    BOR,
    BOXEQ, BOXNE,           // == / != on two boxes: autoboxing caches -128..127 (Character 0..127),
                            // other boxes are distinct objects
};

enum Un : int32_t
{
    INEG, LNEG, DNEG,    // DNEG also negates a float
    INOT, LNOT,          // ~
    ZNOT,                // ! on boolean
    I2L,                 // int64 -> int64 (no-op; kept for clarity)
    L2I,                 // wrap to 32 bits
    I2D,                 // int64 -> double
    D2I, D2L,            // Java's saturating float -> integer conversion
    D2F,                 // round to float
    I2F,                 // int64 -> float-rounded double
    I2B, I2S, I2C,       // wrap to byte / short / char
    ISTR,                // int64 -> decimal string
    DSTR, FSTR,          // Double.toString / Float.toString
    CSTR,                // char (int64 unit) -> string
    ZSTR,                // boolean -> "true" / "false"
    SSTR,                // String -> itself, null -> "null"
    VSTR,                // a boxed value / String / null -> String.valueOf (not instances)
    ALEN,                // array.length (NullPointerException)
    NONNULL,             // NullPointerException unless non-null; the value itself
    UNBOXC,              // Character box -> char (NullPointerException)
    BOXC_UNUSED,         // (was: char -> one-character string)
    SLEN,                // String.length() (NullPointerException)
    BOXJ, BOXS, BOXB,    // long / short / byte -> Long / Short / Byte box (valueOf: cached for -128..127)
    BOXC,                // char -> Character box (valueOf: cached for 0..127)
    UNBOXN,              // Long / Short / Byte box -> its value (NullPointerException)
};

} // namespace jop
