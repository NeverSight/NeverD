import fixture.*;
public class SyntheticInterfaceRootHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned synthetic interface roots");}
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),scenario=Integer.parseInt(args[2]);
        String write=args[3],defaultsName=args[4];
        boolean defaults=scenario>=7&&scenario<=10,missing=scenario==6||scenario==8;
        int fail=scenario==4||scenario==9?1:scenario==5||scenario==10?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            Api receiver=missing?null:kind==0?new Alpha():new Beta();
            Object token=scenario==1?null:scenario==2?new Object[]{new Object(),null}:new Object();
            long a=scenario%2==0?Long.MIN_VALUE:0x12345678fedcba98L,b=Long.MAX_VALUE-scenario,c=~a,d=0x1020304050607080L+attempt;
            int raw=scenario==1?0x80000000:scenario==2?0x7fc01234:scenario==3?0x7f800000:scenario==11?0xffc04321:0x3f800001;
            float scale=Float.intBitsToFloat(raw);int mode=0x13570000+scenario;
            Trace.calls=0;Trace.tag=0;Trace.events=0;Trace.token=null;Trace.a=0;Trace.b=0;Trace.c=0;Trace.d=0;Trace.bits=0;Trace.mode=0;Trace.fail=fail;
            Trace.failure=fail==1?new IllegalStateException("owned runtime"):new AssertionError("owned error");
            Throwable failure=null;
            try{SyntheticInterfaceRootCaller.invoke(route,receiver,kind,defaults,defaults?defaultsName:write,token,a,b,c,d,scale,mode);}catch(Throwable error){failure=error;}
            if(missing) {
                require(Trace.calls==0&&Trace.tag==0&&Trace.events==0&&Trace.token==null&&Trace.a==0&&Trace.b==0&&Trace.c==0&&Trace.d==0&&Trace.bits==0&&Trace.mode==0);
                require(route==3?failure==null:failure instanceof NullPointerException);
            } else {
                int tag=defaults?7:kind+1;boolean stored=fail!=1;
                require(Trace.calls==1&&Trace.tag==tag&&Trace.events==(fail==1?tag:tag*10+9));
                require(Trace.token==(stored?token:null)&&Trace.a==(stored?a:0)&&Trace.b==(stored?b:0)&&Trace.c==(stored?c:0)&&Trace.d==(stored?d:0)&&Trace.bits==(stored?raw:0)&&Trace.mode==(stored?mode:0));
                require(failure==(fail==0?null:Trace.failure));
            }
        }
        System.out.println("attempts=2,synthetic-interface-dispatch-values-identities-failures=matched");
    }
}
