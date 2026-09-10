// Runs smoke.mjs under node or bun and asserts on its output.
//
//   node run.mjs
//   bun  run.mjs

import createSmokeModule from './smoke.mjs';

const lines = [];

const runtime = typeof Bun !== 'undefined' ? 'bun' : 'node';

await createSmokeModule( {
    arguments: [ '--board=/work/proj/board.kicad_pcb' ],
    print: ( text ) => { lines.push( text ); console.log( text ); },
    printErr: ( text ) => { lines.push( text ); console.error( text ); },
} );

const failures = lines.filter( ( l ) => l.startsWith( 'FAIL' ) );
const oks = lines.filter( ( l ) => l.startsWith( 'OK' ) );
const allOk = lines.some( ( l ) => l.trim() === 'ALL OK' );

console.log( `-- ${runtime}: ${oks.length} checks OK, ${failures.length} failed` );

if ( failures.length > 0 || !allOk )
{
    console.error( `-- ${runtime}: SMOKE TEST FAILED` );
    process.exit( 1 );
}

console.log( `-- ${runtime}: SMOKE TEST OK` );
process.exit( 0 );
