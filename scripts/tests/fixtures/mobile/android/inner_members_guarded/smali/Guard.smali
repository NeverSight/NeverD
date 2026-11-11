.class public Lfixture/Guard;
.super Ljava/lang/Object;
.field public static trace:I

.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static check(ILjava/lang/String;)V
    .registers 4
    sget v0, Lfixture/Guard;->trace:I
    mul-int/lit8 v0, v0, 10
    const-string v1, "first"
    if-ne p1, v1, :second
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/Guard;->trace:I
    if-gez p0, :done
    goto :failure
    :second
    add-int/lit8 v0, v0, 2
    sput v0, Lfixture/Guard;->trace:I
    if-nez p0, :done
    :failure
    new-instance v0, Ljava/lang/IllegalArgumentException;
    invoke-direct {v0, p1}, Ljava/lang/IllegalArgumentException;-><init>(Ljava/lang/String;)V
    throw v0
    :done
    return-void
.end method
