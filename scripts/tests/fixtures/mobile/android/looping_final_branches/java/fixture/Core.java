package fixture;

public final class Core {
    private final Token[] values;
    private final String text;
    public Core(Token... input) {
        Trace.partial=this;
        Trace.hit(0,this,input,null,-1,null);
        if(input.length==0) {
            this.values=Trace.empty;
            Trace.hit(1,this,input,this.values,-1,null);
            this.text=Trace.emptyText;
            Trace.hit(2,this,input,this.values,-1,this.text);
        } else {
            Token[] copy=new Token[input.length];
            StringBuilder builder=new StringBuilder();
            Trace.builder=builder;
            for(int i=0;i<input.length;++i) {
                Trace.index=i;
                Token made=(Token)input[i].clone();
                copy[i]=made;
                builder.append(made.id).append('|');
                Trace.hit(5,this,input,copy,i,made);
            }
            this.values=copy;
            Trace.hit(1,this,input,copy,-1,null);
            this.text=Trace.text(this,input,copy,builder);
            Trace.hit(2,this,input,copy,-1,this.text);
        }
    }
    public Token[] getValues(){return values;}
    public String getText(){return text;}
}
