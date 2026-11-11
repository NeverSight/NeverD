.class public final Lfixture/Client;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static make(JZIZ)Lfixture/Target;
    .registers 15
    new-instance v0, Lfixture/Target;
    move/from16 v6, p2
    move-wide/from16 v4, p0
    move/from16 v8, p4
    move/from16 v7, p3
    move-object/from16 v3, v0
    invoke-direct/range {v3 .. v8}, Lfixture/Target;-><init>(JZIZ)V
    move-wide/from16 v0, v4
    return-object v3
.end method
