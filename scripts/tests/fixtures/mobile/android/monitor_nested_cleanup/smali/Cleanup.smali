.class public final Lfixture/Cleanup;
.super Ljava/lang/Object;
.field public static observer:Ljava/util/function/IntConsumer;

.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static hit(I)V
    .registers 2
    sget-object v0, Lfixture/Cleanup;->observer:Ljava/util/function/IntConsumer;
    if-eqz v0, :done
    invoke-interface {v0, p0}, Ljava/util/function/IntConsumer;->accept(I)V
    :done
    return-void
.end method
