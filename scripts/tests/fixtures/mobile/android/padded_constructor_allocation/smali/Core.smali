.class public final Lfixture/Core;
.super Ljava/lang/Object;
.source "Core.java"
.field public final value:Lfixture/Value;

.method public constructor <init>(Lfixture/Value;)V
    .registers 4
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/Core;->value:Lfixture/Value;
    const/4 v0, 0x2
    filled-new-array {p1}, [Ljava/lang/Object;
    move-result-object v1
    invoke-static {v0, p0, v1}, Lfixture/Trace;->hit(ILjava/lang/Object;[Ljava/lang/Object;)V
    return-void
.end method

.method public constructor <init>(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V
    .registers 14
    new-instance v0, Lfixture/Value;
    nop
    nop
    nop
    nop
    nop
    nop
    move-object/from16 v1, p1
    move-object/from16 v2, p2
    move-object/from16 v3, p3
    move-object/from16 v4, p4
    move-object/from16 v5, p5
    move-object/from16 v6, p6
    invoke-direct/range {v0 .. v6}, Lfixture/Value;-><init>(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V
    move-object v1, v0
    move-object/from16 v0, p0
    invoke-direct {v0, v1}, Lfixture/Core;-><init>(Lfixture/Value;)V
    return-void
.end method
