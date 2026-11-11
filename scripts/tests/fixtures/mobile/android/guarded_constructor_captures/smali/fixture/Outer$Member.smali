.class public final Lfixture/Outer$Member;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/EnclosingClass;
    value = Lfixture/Outer;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
    accessFlags = 17
    name = "Member"
.end annotation
.field final synthetic this$0:Lfixture/Outer;
.field public final first:I
.field public final second:I
.field public final third:I
.method public constructor <init>(Lfixture/Outer;III)V
    .registers 6
    iput-object p1, p0, Lfixture/Outer$Member;->this$0:Lfixture/Outer;
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p2, p0, Lfixture/Outer$Member;->first:I
    iput p3, p0, Lfixture/Outer$Member;->second:I
    iput p4, p0, Lfixture/Outer$Member;->third:I
    sget v0, Lfixture/Outer;->constructions:I
    add-int/lit8 v0, v0, 1
    sput v0, Lfixture/Outer;->constructions:I
    return-void
.end method
.method public synthetic constructor <init>(Lfixture/Outer;IIIILjava/lang/Object;)V
    .registers 8
    and-int/lit8 p6, p5, 1
    const/4 v0, 0
    if-eqz p6, :second
    move p2, v0
    :second
    and-int/lit8 p6, p5, 2
    if-eqz p6, :third
    move p3, v0
    :third
    and-int/lit8 p5, p5, 4
    if-eqz p5, :initialize
    invoke-virtual {p1}, Lfixture/Outer;->limit()I
    move-result p4
    :initialize
    invoke-direct {p0, p1, p2, p3, p4}, Lfixture/Outer$Member;-><init>(Lfixture/Outer;III)V
    return-void
.end method

.method public owner()Lfixture/Outer;
    .registers 2
    iget-object v0, p0, Lfixture/Outer$Member;->this$0:Lfixture/Outer;
    return-object v0
.end method
