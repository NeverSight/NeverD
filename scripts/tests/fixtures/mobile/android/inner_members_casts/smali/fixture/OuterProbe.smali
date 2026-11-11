.class public Lfixture/OuterProbe;
.super Ljava/lang/Object;
.field public static constructions:I
.field public final observed:I
.field public final passed:Ljava/lang/String;
.field public final owner:Ljava/lang/Object;

.method public constructor <init>(Ljava/lang/String;Ljava/lang/Object;)V
    .registers 4
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget v0, Lfixture/InnerHost;->trace:I
    mul-int/lit8 v0, v0, 10
    add-int/lit8 v0, v0, 3
    sput v0, Lfixture/InnerHost;->trace:I
    sget v0, Lfixture/OuterProbe;->constructions:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/OuterProbe;->constructions:I
    invoke-virtual {p0}, Lfixture/OuterProbe;->readOuter()I
    move-result v0
    iput v0, p0, Lfixture/OuterProbe;->observed:I
    iput-object p1, p0, Lfixture/OuterProbe;->passed:Ljava/lang/String;
    iput-object p2, p0, Lfixture/OuterProbe;->owner:Ljava/lang/Object;
    return-void
.end method

.method public readOuter()I
    .registers 2
    const/4 v0, 0
    return v0
.end method
