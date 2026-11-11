.class public Lfixture/StaticValues;
.super Ljava/lang/Object;
.field public static initial:I
.field public static wide:J
.field public static token:Lfixture/InnerHost;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method static constructor <clinit>()V
    .registers 2
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/InnerHost;->trace:I
    sget-boolean v0, Lfixture/InnerHost;->fail:Z
    if-eqz v0, :initialize
    sget-object v0, Lfixture/InnerHost;->failure:Ljava/lang/RuntimeException;
    throw v0
    :initialize
    const/16 v0, 11
    sput v0, Lfixture/StaticValues;->initial:I
    const-wide v0, 0x12345678abcdef01
    sput-wide v0, Lfixture/StaticValues;->wide:J
    new-instance v0, Lfixture/InnerHost;
    const/16 v1, 91
    invoke-direct {v0, v1}, Lfixture/InnerHost;-><init>(I)V
    sput-object v0, Lfixture/StaticValues;->token:Lfixture/InnerHost;
    return-void
.end method
