package fixture;

public class Trace {
    public static Trace hook;
    public static Core partial;
    public static Token[] input,empty;
    public static String emptyText,textReply;
    public static Object cloneReply;
    public static boolean overrideClone;
    public static int index;
    public static StringBuilder builder;
    public Trace(){}
    public void observe(int phase,Core subject,Token[] input,Token[] copy,int index,Object value){}
    public static void hit(int phase,Core subject,Token[] input,Token[] copy,int index,Object value){
        Trace current=hook;
        if(current!=null)current.observe(phase,subject,input,copy,index,value);
    }
    public static Object clone(Token item){
        hit(3,partial,input,null,index,item);
        if(overrideClone)return cloneReply;
        int copiedId=item.id;
        return new Token(copiedId);
    }
    public static String text(Core subject,Token[] input,Token[] copy,StringBuilder builder){
        hit(4,subject,input,copy,-1,builder);
        return textReply;
    }
}
