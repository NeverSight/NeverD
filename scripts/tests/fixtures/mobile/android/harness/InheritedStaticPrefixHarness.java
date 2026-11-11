import fixture.*;
import java.lang.reflect.InvocationTargetException;

public final class InheritedStaticPrefixHarness {
    static void require(boolean ok, String detail) {
        if (!ok) throw new AssertionError("owned inherited static prefix: " + detail);
    }
    static Core call(int route, long input) throws Throwable {
        try {
            if (route==0) return new Core(input);
            if (route==1) return InheritedStaticPrefixCaller.call(input);
            if (route==2) return Core.class.getConstructor(long.class).newInstance(input);
            return new Core();
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        int[] codes={Integer.MIN_VALUE,Integer.MAX_VALUE,0,1,-1,37,-91,0x12345678};
        long[] wides={Long.MIN_VALUE,Long.MAX_VALUE,0,1,-1,0x100000001L,-0x123456789abcdefL,0x76543210fedcba98L};
        boolean poisoned=false;
        Core previous=null;
        for (int attempt=0;attempt<2;attempt++) {
            int edge=(mode+attempt)%codes.length;
            int code=codes[edge]; long wide=wides[edge], input=0x7fedcba987654321L-attempt;
            Object token=mode==22?null:new Object(), replacement=new Object();
            int failAt=mode>=8&&mode<14?mode-4:mode>=14&&mode<17?mode-13:
                mode>=17&&mode<20?mode-16:mode==24?5:mode==25?7:mode==26?9:0;
            RuntimeException original=new IllegalStateException("owned constructor failure");
            Error fatal=new AssertionError("owned initialization or constructor error");
            boolean isFatal=mode>=14&&mode<17||mode==25||mode==26;
            Trace.seedCode=code;Trace.seedWide=wide;Trace.seedToken=token;
            if (attempt!=0&&!poisoned) Ancestor.replace(code,wide,token);
            Trace.events=0;Trace.partial=null;Trace.failAt=failAt;Trace.failure=original;
            Trace.fatal=isFatal?fatal:null;
            Trace.observer=()->{
                long last=Trace.events%10;
                if ((mode==20||mode==24)&&last==5||mode==21&&last==4)
                    Ancestor.replace(code^0x13572468,wide^0x123456789abcdefL,replacement);
            };
            long expectedEvents=0;
            boolean published=false, firstFields=false, lastFields=false, afterField=false;
            if (!poisoned) {
                for (int event=attempt==0?1:4;event<=9;event++) {
                    if (event==4) published=true;
                    if (event==6) firstFields=true;
                    if (event==7) lastFields=true;
                    if (event==9) afterField=true;
                    expectedEvents=expectedEvents*10+event;
                    if (event==failAt) break;
                }
            }
            Core actual=null;Throwable thrown=null;
            try {actual=call(route,input);} catch(Throwable failure) {thrown=failure;}
            if (poisoned) require(thrown instanceof NoClassDefFoundError,"erroneous class stays erroneous");
            else if (failAt!=0) {
                if (attempt==0&&failAt<=3) {
                    if (isFatal) require(thrown==fatal,"initialization error identity");
                    else require(thrown instanceof ExceptionInInitializerError&&thrown.getCause()==original,"initializer cause identity");
                } else require(thrown==(isFatal?fatal:original),"constructor failure identity");
                require(actual==null,"failure result");
            } else require(thrown==null&&actual!=null&&actual!=previous,"successful distinct object");
            require(Trace.events==expectedEvents,"initialization and constructor order");
            require((Trace.partial!=null)==published,"partial publication");
            if (published) {
                Parent partial=(Parent)Trace.partial;
                require(partial.inputSnapshot==(firstFields?(route==3?wide:input):0),"input snapshot");
                require(partial.codeSnapshot==(firstFields?code:0),"integer snapshot");
                require(partial.wideSnapshot==(lastFields?wide:0),"wide snapshot");
                require(partial.tokenSnapshot==(lastFields?token:null),"reference identity");
                require(((Core)partial).after==(afterField?wide:0),"child write point");
                if (actual!=null) require(actual==partial,"constructor identity");
            }
            previous=actual;
            if (attempt==0&&failAt>=1&&failAt<=3) poisoned=true;
        }
        System.out.println("attempts=2,inherited-static-prefix=matched");
    }
}
