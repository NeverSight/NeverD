.class public final enum Lfixture/Mode;
.super Ljava/lang/Enum;
.annotation system Ldalvik/annotation/Signature;
    value = { "Ljava/lang/Enum<Lfixture/Mode;>;" }
.end annotation
.field public static final enum FIRST:Lfixture/Mode;
.field public static final enum SECOND:Lfixture/Mode;
.field private static final synthetic $VALUES:[Lfixture/Mode;
.field private static final TABLE:Lfixture/EntryTable;
.field private final bias:I

.method private constructor <init>(Ljava/lang/String;II)V
    .registers 4
    .annotation system Ldalvik/annotation/Signature;
        value = { "(I)V" }
    .end annotation
    invoke-direct {p0, p1, p2}, Ljava/lang/Enum;-><init>(Ljava/lang/String;I)V
    iput p3, p0, Lfixture/Mode;->bias:I
    return-void
.end method

.method private static synthetic $values()[Lfixture/Mode;
    .registers 2
    sget-object v0, Lfixture/Mode;->FIRST:Lfixture/Mode;
    sget-object v1, Lfixture/Mode;->SECOND:Lfixture/Mode;
    filled-new-array {v0, v1}, [Lfixture/Mode;
    move-result-object v0
    return-object v0
.end method

.method static constructor <clinit>()V
    .registers 4
    new-instance v0, Lfixture/Mode;
    const-string v1, "FIRST"
    const/4 v2, 0
    const/4 v3, 3
    invoke-direct {v0, v1, v2, v3}, Lfixture/Mode;-><init>(Ljava/lang/String;II)V
    sput-object v0, Lfixture/Mode;->FIRST:Lfixture/Mode;
    new-instance v0, Lfixture/Mode;
    const-string v1, "SECOND"
    const/4 v2, 1
    const/4 v3, 7
    invoke-direct {v0, v1, v2, v3}, Lfixture/Mode;-><init>(Ljava/lang/String;II)V
    sput-object v0, Lfixture/Mode;->SECOND:Lfixture/Mode;
    invoke-static {}, Lfixture/Mode;->$values()[Lfixture/Mode;
    move-result-object v0
    sput-object v0, Lfixture/Mode;->$VALUES:[Lfixture/Mode;
    sget-object v1, Lfixture/Mode;->$VALUES:[Lfixture/Mode;
    invoke-static {v1}, Lfixture/EntryTable;->create([Lfixture/Mode;)Lfixture/EntryTable;
    move-result-object v0
    sput-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;
    return-void
.end method

.method public static values()[Lfixture/Mode;
    .registers 1
    sget-object v0, Lfixture/Mode;->$VALUES:[Lfixture/Mode;
    invoke-virtual {v0}, Ljava/lang/Object;->clone()Ljava/lang/Object;
    move-result-object v0
    check-cast v0, [Lfixture/Mode;
    return-object v0
.end method

.method public static valueOf(Ljava/lang/String;)Lfixture/Mode;
    .registers 2
    const-class v0, Lfixture/Mode;
    invoke-static {v0, p0}, Ljava/lang/Enum;->valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;
    move-result-object v0
    check-cast v0, Lfixture/Mode;
    return-object v0
.end method

.method public static entries()Lfixture/EntryTable;
    .registers 1
    sget-object v0, Lfixture/Mode;->TABLE:Lfixture/EntryTable;
    return-object v0
.end method

.method public score(I)I
    .registers 3
    iget v0, p0, Lfixture/Mode;->bias:I
    add-int v0, v0, p1
    return v0
.end method
