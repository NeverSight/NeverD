package fixture;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <T,V extends Bound> Carrier<T> create(T first, Converter<T,V> converter,
            Carrier<T> input, T second, String label, Callback<? super T,Unit> callback, int kind) {
        Trace.hit(1);
        Trace.first=first; Trace.second=second; Trace.converter=converter;
        Trace.input=input; Trace.callback=callback; Trace.label=label;
        Trace.hit(2);
        V middle=converter.convert(first);
        Trace.converted=middle; Trace.hit(4);
        Unit observed=callback.apply(second);
        Trace.callbackResult=observed; Trace.hit(6);
        if(kind==1)return (Carrier<T>)Trace.supplied;
        if(kind==2)return null;
        if(kind==3)return input;
        Trace.hit(7);
        return new Carrier<T>(first,second);
    }
}
