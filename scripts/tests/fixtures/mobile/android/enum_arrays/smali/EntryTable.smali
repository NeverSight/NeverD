.class public final Lfixture/EntryTable;
.super Ljava/lang/Object;
.field private final items:[Lfixture/Mode;

.method public static create([Lfixture/Mode;)Lfixture/EntryTable;
    .registers 2
    new-instance v0, Lfixture/EntryTable;
    invoke-direct {v0, p0}, Lfixture/EntryTable;-><init>([Lfixture/Mode;)V
    return-object v0
.end method

.method public constructor <init>([Lfixture/Mode;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    return-void
.end method

.method public size()I
    .registers 2
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    array-length v0, v0
    return v0
.end method

.method public get(I)Lfixture/Mode;
    .registers 4
    if-ltz p1, :missing
    iget-object v0, p0, Lfixture/EntryTable;->items:[Lfixture/Mode;
    array-length v1, v0
    if-ge p1, v1, :missing
    aget-object v0, v0, p1
    return-object v0
    :missing
    const/4 v0, 0
    return-object v0
.end method
