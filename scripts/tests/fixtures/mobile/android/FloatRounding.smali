.class public Lfixture/FloatRounding;
.super Ljava/lang/Object;

.field public static field:F = 1.000000059604644775390625000000000000000000000000001f

.method public static scalar()F
    .registers 1
    const v0, 1.000000059604644775390625000000000000000000000000001f
    return v0
.end method

.method public static array()[F
    .registers 2
    const/16 v1, 10
    new-array v0, v1, [F
    fill-array-data v0, :data
    return-object v0

    :data
    .array-data 4
        1.000000059604644775390625000000000000000000000000001f
        1.000000178813934326171874999999999999999999999999999f
        -1.000000059604644775390625000000000000000000000000001f
        -1.000000178813934326171874999999999999999999999999999f
        0x1.fffffep127f
        0x1p-149f
        -0.0f
        1.000000059604644775390625f
        -1.000000059604644775390625f
        1.000000178813934326171875f
    .end array-data
.end method
