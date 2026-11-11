package fixture;

public interface Rule<Owner> {
    int compare(Owner first, Owner second);

    @SuppressWarnings("unchecked")
    static <T, U> Rule<T> bind(Mapper<? super T, ? extends U> mapper,
                              Rule<? super U> rule) {
        Trace.hit(1);
        Trace.mapper = mapper;
        Trace.rule = rule;
        Trace.hit(2);
        Trace.action.run();
        Trace.hit(3);
        return (Rule<T>) Trace.result;
    }
}
