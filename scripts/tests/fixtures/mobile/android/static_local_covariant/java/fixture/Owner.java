package fixture;
public final class Owner {
    public static Api create(Narrow seed,float first,float second) {
        Trace.seed=seed;Trace.first=first;Trace.second=second;Trace.hit(1);
        return new Api() {
            private final Narrow[] values=new Narrow[]{seed,null};
            public Narrow get(int index) { Trace.hit(2);return values[index]; }
        };
    }
}
