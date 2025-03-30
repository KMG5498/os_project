#ifndef FIXED_POINT_H
#define FIXED_POINT_H

typedef int32_t FP;

#define fp_fr 14
#define fp_f (1 << fp_fr)

#define FP(x) ((x) << fp_fr)
#define FP2INT(x) ((x) >> fp_fr)
#define ADD(x, y) ((x) + (y))
#define SUB(x, y) ((x) - (y))
#define MUL(x, y) (((int64_t)(x)) * (y) / fp_f)
#define DIV(x, y) (((int64_t)(x)) * fp_f / (y))

#endif
