.class public final Lfixture/Client;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static make(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Lfixture/Node;
    .registers 21
    :allocation_start
    new-instance v0, Lfixture/Node;
    :allocation_end
    move-object v9, p0
    move-object/from16 v10, p1
    move-object/from16 v11, p2
    move-object/from16 v12, p3
    move-object/from16 v13, p4
    move-object/from16 v14, p5
    move-object v8, v0
    :constructor_start
    invoke-direct/range {v8 .. v14}, Lfixture/Node;-><init>(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V
    :constructor_end
    return-object v0

    :allocation_failure
    move-exception v1
    goto :cleanup
    :constructor_failure
    move-exception v1
    move-object p0, v9
    move-object/from16 p1, v10
    move-object/from16 p2, v11
    move-object/from16 p3, v12
    move-object/from16 p4, v13
    move-object/from16 p5, v14
    :cleanup
    sput-object v1, Lfixture/Trace;->caught:Ljava/lang/RuntimeException;
    filled-new-array/range {p0 .. p5}, [Ljava/lang/Object;
    move-result-object v2
    sput-object v2, Lfixture/Trace;->arguments:[Ljava/lang/Object;
    const/4 v2, 3
    invoke-static {v2}, Lfixture/Trace;->hit(I)V
    const/4 v2, 0
    return-object v2
    .catch Ljava/lang/IllegalArgumentException; {:allocation_start .. :allocation_end} :allocation_failure
    .catch Ljava/lang/IllegalArgumentException; {:constructor_start .. :constructor_end} :constructor_failure
.end method
