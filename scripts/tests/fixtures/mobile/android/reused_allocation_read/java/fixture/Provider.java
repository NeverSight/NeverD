package fixture;
public class Provider {
    public static volatile String field;
    public static Value cast(){return new Value((CharSequence)(Object)"owned-first");}
    public static Value distinct(){return new Value("owned-first","owned-second","owned-third");}
    public static Value constant(){return new Value("owned-first","owned-shared","owned-shared");}
    public static Value volatileRead(){String cached;return new Value(cached=field,Changer.value,cached);}
}
