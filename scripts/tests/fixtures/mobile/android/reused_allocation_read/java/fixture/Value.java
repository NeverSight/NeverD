package fixture;
public final class Value {
    static {Trace.targetInitializations++;}
    public final String first,second,third;
    public Value(CharSequence first){this((String)first,"owned-second","owned-third");}
    public Value(String first,String second,String third){this.first=first;this.second=second;this.third=third;Trace.calls++;Trace.receiver=this;Trace.hit(1);Trace.action.run();Trace.hit(2);}
}
