.class public final Lfixture/Carrier;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
    value = { "<T:Ljava/lang/Object;>Ljava/lang/Object;" }
.end annotation
.field private final value:Ljava/lang/Object;

.method private synthetic constructor <init>(Ljava/lang/Object;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lfixture/Carrier;->value:Ljava/lang/Object;
    return-void
.end method

.method public static final synthetic wrap(Ljava/lang/Object;)Lfixture/Carrier;
    .registers 2
    new-instance v0, Lfixture/Carrier;
    invoke-direct {v0, p0}, Lfixture/Carrier;-><init>(Ljava/lang/Object;)V
    return-object v0
.end method

.method public final synthetic unwrap()Ljava/lang/Object;
    .registers 2
    iget-object v0, p0, Lfixture/Carrier;->value:Ljava/lang/Object;
    return-object v0
.end method

.method public static use(Ljava/lang/Object;Ljava/util/List;)Ljava/lang/Object;
    .registers 2
    .annotation system Ldalvik/annotation/Signature;
        value = { "(Ljava/lang/Object;Ljava/util/List<TT;>;)Ljava/lang/Object;" }
    .end annotation
    return-object p0
.end method
