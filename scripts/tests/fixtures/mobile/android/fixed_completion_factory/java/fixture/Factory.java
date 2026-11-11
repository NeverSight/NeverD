package fixture;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <R> Completion<Leaf> unary(Unary<? super Completion<? super R>,? extends Object> block,Completion<? super R> completion) {
        return Trace.transfer(block,null,completion,0);
    }
    @SuppressWarnings("unchecked")
    public static <T,R> Completion<Leaf> seeded(Binary<? super T,? super Completion<? super R>,? extends Object> block,T receiver,Completion<? super R> completion) {
        return Trace.transfer(block,receiver,completion,1);
    }
}
