package fixture;
public final class Core {
    public final Token value;
    public final Object input;
    public final long word;
    public final String label;
    private Core(Token token,Object input,long word,String label) {
        Trace.partial=this;Trace.hit(4);this.value=token;Trace.hit(5);
        this.input=input;this.word=word;this.label=label;Trace.hit(6);
    }
    public Core(Object input,long word,String label) {
        this(new Token(null,null,3,null),input,word,label);
    }
}
