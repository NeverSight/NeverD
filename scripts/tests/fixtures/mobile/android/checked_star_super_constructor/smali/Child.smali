.class public final Lfixture/Child;
.super Lfixture/Base;

.field public final first:Ljava/lang/Object;
.field public final second:Ljava/lang/Object;

.method static constructor <clinit>()V
    .registers 1
    const/4 v0, 2
    invoke-static {v0}, Lfixture/Trace;->step(I)V
    return-void
.end method

.method public constructor <init>(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V
    .registers 4
    iput-object p2, p0, Lfixture/Child;->first:Ljava/lang/Object;
    iput-object p3, p0, Lfixture/Child;->second:Ljava/lang/Object;
    move-object p2, p1
    check-cast p2, Lfixture/Context$Key;
    invoke-direct {p0, p2}, Lfixture/Base;-><init>(Lfixture/Context$Key;)V
    iget-object p1, p0, Lfixture/Base;->key:Lfixture/Context$Key;
    sput-object p1, Lfixture/Trace;->seenFinal:Ljava/lang/Object;
    sget p1, Lfixture/Trace;->mask:I
    or-int/lit8 p1, p1, 4
    sput p1, Lfixture/Trace;->mask:I
    const/4 p1, 5
    invoke-static {p1}, Lfixture/Trace;->step(I)V
    return-void
.end method

.method public left()Ljava/lang/Object;
    .registers 2
    iget-object v0, p0, Lfixture/Child;->first:Ljava/lang/Object;
    return-object v0
.end method

.method public right()Ljava/lang/Object;
    .registers 2
    iget-object v0, p0, Lfixture/Child;->second:Ljava/lang/Object;
    return-object v0
.end method
