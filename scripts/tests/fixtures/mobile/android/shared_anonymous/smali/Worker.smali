.class public final Lfixture/SharedHost$Worker;
.super Ljava/lang/Object;

.annotation system Ldalvik/annotation/EnclosingMethod;
    value = Lfixture/SharedHost;->make(I)Ljava/lang/Object;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 0x10
    name = null
.end annotation

.field static count:I
.field public final value:I

.method public constructor <init>(I)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lfixture/SharedHost$Worker;->value:I
    sget v0, Lfixture/SharedHost$Worker;->count:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/SharedHost$Worker;->count:I
    return-void
.end method

.method nested()Ljava/lang/Object;
    .registers 2
    new-instance v0, Lfixture/SharedHost$Worker$Child;
    invoke-direct {v0, p0}, Lfixture/SharedHost$Worker$Child;-><init>(Lfixture/SharedHost$Worker;)V
    return-object v0
.end method
